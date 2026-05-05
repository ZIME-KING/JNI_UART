# QSJNIC 智能座舱底层通信中间件及 SDK 架构白皮书

## 1. 项目概述
QSJNIC 是专为新一代智能座舱打造的高性能、高可靠的 **Android 与 MCU 跨芯片串行通信中间件**。

在现代智能座舱中，收音机应用、车辆信号应用、OTA 升级应用等多个独立进程需要同时与底层 MCU 进行高频交互。为了彻底解决多进程直接抢占同一个串口（`/dev/ttyS7`）导致的数据错乱和死锁问题，本项目采用了 **工业级的 SystemService 隔离代理架构**。

我们将原本的单体 SDK 拆分为：
1. **McuSystemService (服务端)**：作为常驻后台的 Android 系统服务，全局唯一独占串口。
2. **McuSdk (客户端 SDK)**：基于 Android AIDL (Binder IPC) 与服务端通信，为各业务 App 提供隔离、安全、纯异步的标准化 Java/Kotlin API。

---

## 2. 系统架构设计

本系统采用标准的 **C/S (Client-Server) 多进程架构**，从上至下分为四大层级：

```mermaid
graph TD
    subgraph Client Application (客户多进程应用层)
        App1[收音机 App]
        App2[车辆信号 App]
        App3[OTA 升级 App]
    end

    subgraph QSJNIC SDK Layer (AAR)
        SDK[CarSerialSDK]
        BinderProxy[AIDL 客户端代理]
        Listener[回调监听路由]
        SDK --- BinderProxy
        SDK --- Listener
    end

    App1 <-->|调用 API / 接收回调| SDK
    App2 <-->|调用 API / 接收回调| SDK
    App3 <-->|调用 API / 接收回调| SDK

    subgraph Android System Service (系统服务代理层)
        McuService[McuSystemService]
        BinderStub[AIDL 服务端 Stub]
        BinderStub --- McuService
    end

    BinderProxy <-->|Binder IPC 跨进程通信| BinderStub

    subgraph Native Middleware (C/C++ 核心通信引擎)
        JNI_Bridge[JNI 桥接层]
        State_Machine[异步状态机 & 重传引擎]
        RX_Thread[全异步高优解析线程]
        TX_Thread[串口发送与心跳线程]
    end

    McuService <-->|JNI 独占调用| JNI_Bridge
    JNI_Bridge --> TX_Thread
    RX_Thread -->|异步抛出事件| McuService
    
    subgraph Hardware Layer (硬件与底层驱动)
        TTY[/dev/ttyS7 460800 8N1/]
        MCU[车机 MCU]
    end

    TX_Thread -->|写入字节流| TTY
    TTY -->|读取粘包流| RX_Thread
    TTY <--> MCU
```

---

## 3. 核心机制与技术壁垒

### 3.1 工业级的多进程隔离与高并发处理
所有的业务请求（不论来自哪个 App）都通过 Binder 发送至 `McuSystemService`。服务端内部采用队列进行命令的**序列化统一下发**，彻底杜绝了并发写入冲突。同时，来自 MCU 的广播帧（如 ACC 点火、倒车、车速）由服务解析后，精确多播给已注册的相关客户端。

### 3.2 高可靠的异常容错与断线自愈
- **服务端防死锁保活**：C++ 核心引擎内置 Unconditional Heartbeat，自动打破 MCU 启动初期的握手死锁；高达 3000ms 的长时重传窗口完美包容了 MCU 侧的慢响应。
- **客户端自愈重连**：当 SystemService 意外崩溃被系统拉起时，SDK 层通过 `IBinder.DeathRecipient` 自动感知断线，对应用层透明地完成重新绑定（re-bind），保障业务不中断。

### 3.3 端到端极致低延迟
通过预分配缓存和 Zero-Copy 思想优化 JNI 与 Binder 跨进程传递，确保客户端调用 API 到 MCU 物理层收到指令的端到端（End-to-End）系统损耗 **≤ 50ms**。

---

## 4. 客户 SDK 接口定义 (API 规范)

客户无需关心 AIDL 的繁琐实现，只需接入 `McuSdk.aar`，即可直接使用面向上层的同步/异步接口。

### 4.1 SDK 初始化与生命周期

```java
public class CarSerialSDK {
    /**
     * 初始化 SDK，内部自动通过 bindService 绑定到底层 McuSystemService。
     * 支持断线自动重连。
     */
    public static void init(Context context);
    
    /**
     * 释放资源，解绑服务
     */
    public static void deinit();
}
```

### 4.2 业务数据监听 (Listener)
各独立 App 按需注册自己关心的回调接口。系统服务收到 MCU 推送后，通过 IPC 回调至对应 App。

```java
// 车辆基础信号 App 监听
CarSerialSDK.setVehicleCallback(new VehicleCallback() {
    @Override
    public void onLineDetectChanged(boolean isACC, boolean isIllumination, boolean isReverse) {
        // 倒车、ACC点火状态更新
    }

    @Override
    public void onVoltageChanged(int voltageMv) {
        // 电压实时监控
    }
});

// 收音机 App 监听
CarSerialSDK.setRadioCallback(new RadioCallback() {
    @Override
    public void onFrequencyChanged(int band, int freq, int signalStrength) {
        // 收音机自动搜台时的实时频率回调
    }
});
```

### 4.3 基础车控 API (Public)

```java
/**
 * 设置屏幕背光
 * @param percent 亮度百分比 0-100
 * @return 0 表示成功，<0 表示内部 Binder 或 JNI 异常
 */
public static int setBacklight(int percent);

/**
 * 设置功放/天线电源
 * @param on true=开，false=关
 */
public static int setAmpPower(boolean on);
```

### 4.4 收音机 API (Radio)

```java
/**
 * 设置收音机频段与频率
 * @param band 0=FM, 1=AM
 * @param freq 频率值 (如 8750 表示 87.5MHz)
 */
public static int setRadioFreq(int band, int freq);

/**
 * 启动自动搜台 (SEEK)
 * @param seekMode 搜索模式：向前、向后
 */
public static int setRadioSeek(int seekMode, int ptyType);
```

### 4.5 固件升级 API (OTA Update)

```java
/**
 * 发送 MCU 固件数据 (内部由 SystemService 处理粘包与阻塞)
 * 注意：升级期间，SystemService 会自动暂停其他 App 的指令下发以保障升级带宽。
 *
 * @param data 固件二进制分包数据 (单帧最大 1024 字节)
 * @param offset 偏移量
 * @param len 发送长度
 */
public static int sendMcuUpgradeData(byte[] data, int offset, int len);

/**
 * 注册 OTA 升级状态回调
 */
CarSerialSDK.setUpdateCallback(new UpdateCallback() {
    @Override
    public void onUpgradeAck(boolean isAccepted) {
        // MCU 回复是否允许进入升级模式
    }
    
    @Override
    public void onProgressRequest(int expectedIndex) {
        // MCU 索要特定的固件数据包
    }
});
```

---

## 5. 权限与集成规范

1. **服务权限隔离**：为了防止被非授权的恶意第三方应用绑定，`McuSystemService` 在 `AndroidManifest.xml` 中定义了自定义 Signature 权限（例如 `android.permission.QSJNIC_MCU_CONTROL`）。客户的应用需要声明该权限方可通信。
2. **预置安装**：`McuSystemService.apk` 建议作为系统特权应用 (Privileged App) 预置到 Android 固件的 `/system/priv-app/` 目录下，并赋予读写 `/dev/ttyS7` 字符设备节点的 Linux 权限。
3. **日志规范**：SDK 具备基于 `log.level` 系统属性控制的日志系统，在量产发版阶段默认关闭底层 Hex 帧级的 DEBUG 打印。
