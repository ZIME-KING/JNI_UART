# 产品开发需求书 (PRD)

**项目名称**：车载中控系统底层服务封装（JNI/SystemService 通信架构）  
**项目代号**：QSJNIC

---

## 1. 项目背景

本项目基于 Realtek RTD1861 芯片的 Android 车载中控主机。硬件方案公司已提供 BSP 层，并开放了与 MCU 通信的串口设备节点 `/dev/ttyS7`（波特率 460800，8N1）。

MCU 与 ARM 之间的通信协议已在相关文档（`MCU-ARM_v2.xlsx`、`Radio_v2.xlsx`、`Public_v2.xlsx` 等）中完整定义，包含车辆信号（ACC、车速、倒车等）、收音机（FM/AM 调频、RDS 信息）、系统升级等消息类型。

为解决多个应用进程（如收音机 App、车辆信号 App、升级 App）并发访问同一串口导致的竞争冲突问题，本项目采用 **“底层 JNI 串口封装 + SystemService 系统服务隔离 + AIDL/Binder 通信”** 的架构方案：
将串口的独占访问权和 JNI 调用封装在 `McuSystemService`（系统级服务）中，上层 App 统一通过新版 Java SDK（基于 AIDL Binder）与该服务进行通信。

该方案需要重点解决：**串口独占**、**多进程安全与隔离**、**协议完整性（建链、心跳、ACK、重传）** 以及 **端到端通信低延迟** 等核心问题。

---

## 2. 技术方案概述

### 2.1 总体架构

原有 SDK 直接持有 JNI 连接的方案已变更为新增 SystemService 层架构：

```mermaid
graph TD
    subgraph 客户端应用层
        A1[收音机 App]
        A2[车辆信号 App]
        A3[升级 App]
    end
    
    subgraph Java SDK 层 (McuSdk.aar)
        B[Java SDK API / Listener 回调]
        C[AIDL 接口封装 / Binder 通信管理]
        B --- C
    end
    
    A1 -->|调用 API| B
    A2 -->|调用 API| B
    A3 -->|调用 API| B
    
    subgraph Android 系统服务层 (McuSystemService.apk)
        D[McuSystemService <br> 注册到 ServiceManager]
    end
    
    C <-->|AIDL / Binder IPC| D
    
    subgraph Native C++ 层 (libserial.so)
        E[libserial.so 动态库 <br> 串口管理 / 协议封解 / 链路管理 / 消息分发]
    end
    
    D <-->|JNI 独占调用| E
    
    E <-->|open/read/write/ioctl| F[/dev/ttyS7 物理串口]
```

### 2.2 关键技术要求

1. **串口独占与多进程隔离**：`McuSystemService` 由系统启动常驻后台，是唯一初始化并持有 `libserial.so` 和 `/dev/ttyS7` 的组件。多个 App 通过 SDK 的 Binder 访问服务，服务负责对并发请求进行序列化，确保多进程安全，数据不混乱。
2. **AIDL 通信与代理机制**：MCU 通信代理统一下发命令，MCU 主动上报的数据由 SystemService 接收后，通过 AIDL 接口以广播或回调机制分发至各自已注册的客户端。
3. **协议完整实现**：Native 层严格按照协议文档实现帧格式、序列号、校验和、建链、心跳（间隔 ≤3 秒）、ACK 机制。
4. **服务重连与容错机制**：新版 SDK 需自动处理 `bindService` 连接及服务异常重启后的断线重连（处理 `IBinder.DeathRecipient`），对上层应用透明。
5. **升级功能支持**：支持 MCU 固件升级的数据分包传输，并确保升级期间其他业务通信被暂停或缓存。

### 2.3 开发环境要求

* **Native 层**：C++11 或更高版本，CMake 构建。
* **系统服务层**：Android Service，提供可独立安装或预置的 `McuSystemService.apk`。
* **Java SDK 层**：Java/Kotlin，提供供应用导入的 `.aar`（含 AIDL 接口）。
* **编译目标**：`arm64-v8a`，Android API Level 31+。

---

## 3. 功能需求明细

*(表格内容与协议层功能一致，通过 SystemService 透传)*

### 3.1 车辆信号 (Public 类)

| 功能 | 方向 | 说明 |
| :--- | :--- | :--- |
| **获取 ACC/IG 状态** | MCU → ARM | 主动上报，AIDL 广播/回调至 SDK |
| **获取倒车状态** | MCU → ARM | 主动上报，AIDL 广播/回调至 SDK |
| **获取车速** | MCU → ARM | 主动上报，含车速百分比 |
| **获取电池电压** | MCU → ARM | 主动上报或主动查询 |
| **获取 MCU 版本** | MCU → ARM | 主动查询 |
| **设置屏幕背光** | ARM → MCU | 0-100% 亮度设置 |
| **设置功放/天线电源** | ARM → MCU | 开关控制 |
| **MCU 睡眠控制** | ARM → MCU | 系统休眠前要求 MCU 进入休眠 |
| **心跳发送** | ARM → MCU | Native 层定时发送心跳帧维持链路 |

### 3.2 收音机 (Radio 类)

| 功能 | 方向 | 说明 |
| :--- | :--- | :--- |
| **设置波段与频率** | ARM → MCU | FM/AM，频率范围根据协议设定 |
| **自动搜索 (SEEK)** | ARM → MCU | 支持向前/向后/AMS/PTY/TA |
| **TA/AF/立体声/本地开关** | ARM → MCU | 布尔控制 |
| **当前频率及信号强度** | MCU → ARM | 主动上报或查询 |
| **RDS 信息 (PS, PTY, RT)** | MCU → ARM | 回调返回字符串 |

### 3.3 系统升级 (Update 类)

| 功能 | 方向 | 说明 |
| :--- | :--- | :--- |
| **启动升级请求** | ARM → MCU | 通知 MCU 准备接收升级 |
| **下发升级数据** | ARM → MCU | 分包发送，最大 1024 字节/帧，支持 ACK 重传 |
| **结束升级** | ARM → MCU | 升级完成通知 |

> [!WARNING]  
> **注意事项**：升级期间，SystemService 应暂停处理或缓存其他客户端应用（收音机、车辆信号等）的通信请求，升级结束后自动恢复。

### 3.4 链路管理要求 (Native 层)

* **自动建链**：初始化时自动发送 Setup 帧，等待 SetupAck，超时重试 3 次。
* **心跳维持**：建链成功后启动定时器，每隔 2 秒发送心跳帧（FrameType=0x05），MCU 回复 ACK。若连续 3 次无响应，触发自动重连。
* **断线重连**：检测到串口错误或心跳超时后，自动关闭串口并重新打开、重新建链。
* **ACK 处理**：发送需确认的帧时阻塞等待 ACK，超时 200ms，重试 3 次，失败则向上返回错误码。

---

## 4. 接口规范 (基于 AIDL 代理)

Java SDK 层需封装底层的 AIDL/Binder 通信，对应用层暴露友好的 API 接口（向后兼容原 SDK 方法签名）。

```java
// SDK 初始化与服务绑定
public class CarSerialSDK {
    // 初始化 SDK，内部执行 bindService 绑定 McuSystemService
    public static void init(Context context);
    public static void deinit();
    
    // Listener 模式回调注册（封装 AIDL 接口）
    public static void setVehicleCallback(VehicleCallback callback);
    public static void setRadioCallback(RadioCallback callback);
    public static void setUpdateCallback(UpdateCallback callback);
    
    // 主动发送命令（同步/异步接口封装）
    public static int setRadioFreq(int band, int freq); // 0=FM, 1=AM
    public static int setRadioSeek(int seekMode, int ptyType);
    public static int setRadioSwitch(int switchType, boolean on);
    public static int setBacklight(int percent);
    public static int setAmpPower(boolean on);
    public static int sendMcuUpgradeData(byte[] data, int offset, int len);
}
```

> [!NOTE]  
> `IMcuService.aidl` 需定义基础的命令下发和事件回调接口。底层必须实现 Android 权限控制声明，防止未授权应用随意绑定该系统服务访问 MCU。

---

## 5. 调试日志规范

必须实现可通过 Android 系统属性（如 `persist.carserial.log.level`）动态控制的日志开关，生产版本应默认关闭 DEBUG 级别的 Hex 日志输出。

| 级别 | 触发场景 |
| :--- | :--- |
| **ERROR** | 系统调用失败；协议校验失败；重试超过上限；设备断连。 |
| **WARN** | ACK 超时重发；心跳丢失；非致命协议异常。 |
| **INFO** | 设备打开/关闭；建链成功/失败；心跳启停；升级开始/结束等。 |
| **DEBUG** | 收发的原始数据（Hex 格式）；协议帧解析结果；进入/退出关键函数。 |

---

## 6. 交付物清单

1. **McuSystemService.apk**：服务端系统应用，独立安装或可预置到系统镜像中。
2. **预编译 .so 库**：`arm64-v8a` 架构 Native 库（内置在 Service 中或由其加载）。
3. **McuSdk.aar (含 AIDL)**：移除直接 JNI 依赖、基于 Binder 通信的新版 SDK。
4. **Demo App 源码**：演示如何初始化新版 SDK、注册回调并调用的示例工程。
5. **技术文档**：包含《SystemService 集成说明》（系统服务预置、权限配置、AIDL 定义说明）、API 手册、协议对照表及调试指南。

*(注：本系统服务框架的系统镜像预置、配置及系统签名由甲方负责；如因系统签名导致 Service 无法注册，责任归属甲方)*

---

## 7. 验收标准

* **服务隔离与并发**：多个 App 同时通过 SDK 访问 MCU 时，数据不混乱，多进程隔离生效，回调可正确分发至各自注册的客户端。
* **容错与重连**：当 `McuSystemService` 异常重启后，SDK 能自动完成重连动作，上层业务可感知重连事件。
* **端到端延迟性能**：应用层调用 SDK 接口，直到 MCU 真正接收到命令，整个 Binder IPC + JNI 链路的端到端延迟 ≤ 50ms（不含 MCU 处理时间）。
* **安全与权限**：AIDL 接口及系统服务具备正确的权限声明控制。
* **功能与稳定性测试**：连续运行 48 小时，内存和 Binder 句柄无泄漏；各项收音机、车辆信号命令均可正常收发。
* **MCU 升级**：SystemService 可以安全稳妥地完成 MCU 的固件包下发和升级闭环。
