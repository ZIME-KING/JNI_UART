#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <sstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "CarSerialService.h"

namespace {
// carserial_host_tool.exe：Windows/PC 侧联调工具（不依赖 Android）
//
// 主要功能：
// 1) 连接串口（COMx）并走 LinkManager 的建链/心跳/ACK 逻辑；
// 2) 打印链路状态（connected / disconnected）；
// 3) 打印协议消息（MsgType/Cmd/DataLen），并对 Public/Radio/Update 做最小解包展示；
// 4) 支持发起简单的“发频率命令”与“升级流程演示”（用于配合 tools/mock_mcu.py 验证链路闭环）。
//
// 使用建议：
// - 没有真实 MCU：用虚拟串口对 + tools/mock_mcu.py；
// - 有真实 MCU：将 <COMx> 指向 USB-Serial 对应 COM 口即可。
volatile std::sig_atomic_t g_stop = 0;

void onSigInt(int) {
  g_stop = 1;
}

static int u16be(const std::vector<uint8_t>& data, int offset) {
  // 读取 big-endian 的 u16（不足返回 0）
  if (offset < 0 || offset + 2 > static_cast<int>(data.size())) return 0;
  return (static_cast<int>(data[offset]) << 8) | static_cast<int>(data[offset + 1]);
}

static std::string asciiTrimmed(const std::vector<uint8_t>& data, int offset, int len) {
  // 将 payload 的一段当作 ASCII 字符串读取：
  // - 遇到 0 截断
  // - 去除首尾空白字符
  if (offset < 0 || len <= 0 || offset >= static_cast<int>(data.size())) return "";
  int max = offset + len;
  if (max > static_cast<int>(data.size())) max = static_cast<int>(data.size());
  int end = offset;
  while (end < max && data[end] != 0) end++;
  if (end <= offset) return "";
  std::string s(reinterpret_cast<const char*>(&data[offset]), reinterpret_cast<const char*>(&data[end]));
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) start++;
  if (start > 0) s.erase(0, start);
  return s;
}

static std::vector<uint8_t> makeMsg(uint8_t msgType, uint8_t cmd, const std::vector<uint8_t>& data) {
  // 组一个“数据帧”负载：payload = [MsgType][Cmd][Data...]
  std::vector<uint8_t> out;
  out.reserve(data.size() + 2);
  out.push_back(msgType);
  out.push_back(cmd);
  out.insert(out.end(), data.begin(), data.end());
  return out;
}

static bool parseInt(const std::string& s, int* out) {
  if (!out) return false;
  if (s.empty()) return false;
  int base = 10;
  if (s.size() > 2 && (s[0] == '0') && (s[1] == 'x' || s[1] == 'X')) base = 16;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, base);
  if (!end || *end != '\0') return false;
  *out = static_cast<int>(v);
  return true;
}

static bool parseHexBytes(const std::string& s, std::vector<uint8_t>* out) {
  if (!out) return false;
  out->clear();
  std::string hex;
  hex.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(s[i]);
    if (std::isxdigit(ch)) {
      hex.push_back(static_cast<char>(ch));
    }
  }
  if (hex.empty()) return true;
  if (hex.size() % 2 != 0) return false;
  out->reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
      if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
      return -1;
    };
    int hi = nibble(hex[i]);
    int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out->push_back(static_cast<uint8_t>((hi << 4) | lo));
  }
  return true;
}

static void usage() {
  // 命令行参数说明见 usage 输出；Windows 里 Ctrl+C 退出。
  std::cout << "Usage:\n"
            << "  carserial_host_tool.exe <COMx> [--baud <baud>]\n"
            << "    [--send-msg <msgType> <cmd> [--data-hex <hex>] [--data-ascii <text>] [--need-ack <0|1>]]\n"
            << "    [--send-raw <frameType> --payload-hex <hex> [--need-ack <0|1>]]\n"
            << "    [--send-radio-freq <band> <freq>]\n"
            << "    [--update-demo]\n"
            << "\n"
            << "Examples:\n"
            << "  carserial_host_tool.exe COM5 --baud 460800\n"
            << "  carserial_host_tool.exe COM5 --send-msg 0x01 0x80 --data-hex \"01 02 03\" --need-ack 1\n"
            << "  carserial_host_tool.exe COM5 --send-raw 0x06 --payload-hex \"01 0A 00 64\" --need-ack 1\n"
            << "  carserial_host_tool.exe COM5 --send-radio-freq 0 10170\n"
            << "  carserial_host_tool.exe COM5 --update-demo\n";
}
}  // namespace

int main(int argc, char** argv) {
  // 默认参数：COM5 / 460800。
  std::string tty = "COM5";
  int baud = 460800;
  bool updateDemo = false;
  bool sendRadioFreq = false;
  int radioBand = 0;
  int radioFreq = 10170;
  bool sendMsg = false;
  int msgType = 0;
  int msgCmd = 0;
  std::vector<uint8_t> msgData;
  bool sendRaw = false;
  int rawFrameType = 0x06;
  std::vector<uint8_t> rawPayload;
  bool needAck = true;

  // argv[1] 作为串口名（COMx）
  if (argc >= 2 && argv[1] && std::string(argv[1]).size() > 0) tty = argv[1];
  // 解析可选参数
  for (int i = 2; i < argc; i++) {
    const char* a = argv[i];
    if (!a) continue;
    if (std::strcmp(a, "--help") == 0 || std::strcmp(a, "-h") == 0) {
      usage();
      return 0;
    }
    if (std::strcmp(a, "--baud") == 0 && i + 1 < argc) {
      // 指定波特率（默认 460800）
      baud = std::atoi(argv[++i]);
      continue;
    }
    if (std::strcmp(a, "--need-ack") == 0 && i + 1 < argc) {
      int v = std::atoi(argv[++i]);
      needAck = v != 0;
      continue;
    }
    if (std::strcmp(a, "--data-hex") == 0 && i + 1 < argc) {
      std::vector<uint8_t> tmp;
      if (!parseHexBytes(argv[++i], &tmp)) {
        std::cerr << "invalid --data-hex" << std::endl;
        return 2;
      }
      msgData.insert(msgData.end(), tmp.begin(), tmp.end());
      continue;
    }
    if (std::strcmp(a, "--data-ascii") == 0 && i + 1 < argc) {
      const char* s = argv[++i];
      if (s && *s) {
        msgData.insert(msgData.end(), s, s + std::strlen(s));
      }
      continue;
    }
    if (std::strcmp(a, "--send-msg") == 0 && i + 2 < argc) {
      sendMsg = true;
      std::string sType = argv[++i];
      std::string sCmd = argv[++i];
      if (!parseInt(sType, &msgType) || !parseInt(sCmd, &msgCmd)) {
        std::cerr << "invalid --send-msg <msgType> <cmd>" << std::endl;
        return 2;
      }
      continue;
    }
    if (std::strcmp(a, "--send-raw") == 0 && i + 1 < argc) {
      sendRaw = true;
      std::string sFt = argv[++i];
      if (!parseInt(sFt, &rawFrameType)) {
        std::cerr << "invalid --send-raw <frameType>" << std::endl;
        return 2;
      }
      continue;
    }
    if (std::strcmp(a, "--payload-hex") == 0 && i + 1 < argc) {
      if (!parseHexBytes(argv[++i], &rawPayload)) {
        std::cerr << "invalid --payload-hex" << std::endl;
        return 2;
      }
      continue;
    }
    if (std::strcmp(a, "--send-radio-freq") == 0 && i + 2 < argc) {
      // 发送 Radio 设置频率消息（示例命令）：band/freq 由参数给出
      sendRadioFreq = true;
      radioBand = std::atoi(argv[++i]);
      radioFreq = std::atoi(argv[++i]);
      continue;
    }
    if (std::strcmp(a, "--update-demo") == 0) {
      // 发送一套升级流程演示命令：req/begin/data/end
      updateDemo = true;
      continue;
    }
  }

  std::signal(SIGINT, onSigInt);

  auto& svc = carserial::CarSerialService::instance();
  svc.setEventSink([](int eventId, int p1, int p2, int p3, const std::string&, const std::vector<uint8_t>& data) {
    // 事件定义见 CarSerialService.h：
    // 1=链路状态，2=原始帧，3=解析后的 MsgType/Cmd
    if (eventId == 1) {
      std::cout << "[link] connected=" << (p1 == 1 ? "true" : "false") << std::endl;
      return;
    }
    if (eventId == 3) {
      int msgType = (p3 >> 8) & 0xFF;
      int cmd = p3 & 0xFF;
      std::cout << "[msg] seq=" << p1 << " frameType=0x" << std::hex << p2 << std::dec << " msgType=0x" << std::hex
                << msgType << " cmd=0x" << cmd << std::dec << " dataLen=" << data.size();

      // 这里做“最小解包展示”，便于联调时快速肉眼确认字段：
      // - Public(0x01)：ACC/倒车/车速/电压/版本
      // - Radio(0x05)：频率/信号/RDS
      // - Update(0x07)：升级总长度/索引/ACK
      if (msgType == 0x01 && cmd == 0x01 && data.size() >= 1) {
        int p0 = data[0];
        bool accHigh = (p0 & (1 << 1)) != 0;
        bool reverseHigh = (p0 & (1 << 3)) != 0;
        std::cout << " [Public] ACC=" << (accHigh ? "1" : "0") << " Reverse=" << (reverseHigh ? "1" : "0");
      } else if (msgType == 0x01 && cmd == 0x02 && !data.empty()) {
        std::cout << " [Public] MCU=" << asciiTrimmed(data, 0, static_cast<int>(data.size()));
      } else if (msgType == 0x01 && cmd == 0x0A && data.size() >= 2) {
        int raw = u16be(data, 0);
        int speed = raw;
        if (data.size() >= 3) {
          int ratio = data[2] & 0xFF;
          if (ratio >= 50 && ratio <= 200) speed = (raw * ratio) / 100;
        }
        std::cout << " [Public] Speed=" << speed;
      } else if (msgType == 0x01 && cmd == 0x0D && data.size() >= 2) {
        int v = u16be(data, 0);
        int mv = v * 100;
        std::cout << " [Public] Voltage=" << mv << "mV";
      } else if (msgType == 0x05 && cmd == 0x01 && data.size() >= 3) {
        int freq = u16be(data, 0);
        int band = data[2] & 0x0F;
        bool stereo = false;
        bool valid = true;
        if (data.size() >= 5) {
          int p4 = data[4] & 0xFF;
          stereo = (p4 & 0x01) != 0;
          valid = (p4 & 0x80) != 0;
        }
        std::cout << " [Radio] band=" << band << " freq=" << freq << " stereo=" << (stereo ? "1" : "0")
                  << " valid=" << (valid ? "1" : "0");
        if (data.size() >= 7) std::cout << " signal=" << (data[6] & 0xFF);
      } else if (msgType == 0x05 && cmd == 0x02 && data.size() >= 9) {
        std::cout << " [Radio] PS=" << asciiTrimmed(data, 1, 8);
      } else if (msgType == 0x05 && cmd == 0x03 && !data.empty()) {
        int max = static_cast<int>(data.size());
        if (max > 64) max = 64;
        std::cout << " [Radio] RT=" << asciiTrimmed(data, 0, max);
      } else if (msgType == 0x07 && cmd == 0x02 && data.size() >= 2) {
        std::cout << " [Update] total=" << u16be(data, 0);
      } else if (msgType == 0x07 && cmd == 0x03 && data.size() >= 2) {
        std::cout << " [Update] index=" << u16be(data, 0);
      } else if (msgType == 0x07 && cmd == 0x01) {
        int st = data.empty() ? 0 : (data[0] & 0xFF);
        std::cout << " [Update] ack=" << st;
      }

      std::cout << std::endl;
      return;
    }
    if (eventId == 2) {
      // 原始帧打印：这里只打印长度信息；更详细的 TX/RX hex 在 LinkManager.cpp 里按日志级别输出
      std::cout << "[raw] seq=" << p1 << " frameType=0x" << std::hex << p2 << std::dec << " payloadLen=" << p3
                << std::endl;
    }
  });

  std::cout << "starting host tool, port=" << tty << " baud=" << baud << ", Ctrl+C to stop" << std::endl;
  if (!svc.init(tty, baud)) {
    std::cerr << "init failed, please check serial port and permission." << std::endl;
    return 1;
  }

  if (sendRadioFreq) {
    // 发送 Radio 设置频率命令（MsgType=0x05, Cmd=0x80），具体字段含义参考协议表。
    std::vector<uint8_t> data;
    data.push_back(static_cast<uint8_t>((radioFreq >> 8) & 0xFF));
    data.push_back(static_cast<uint8_t>(radioFreq & 0xFF));
    data.push_back(static_cast<uint8_t>(radioBand & 0xFF));
    std::vector<uint8_t> payload = makeMsg(0x05, 0x80, data);
    svc.sendRaw(0x06, payload, needAck);
  }

  if (updateDemo) {
    // 发送一套升级流程演示：
    // - 0x80 req
    // - 0x81 begin
    // - 0x82 data（这里发送 5 次小数据块）
    // - 0x83 end
    svc.sendRaw(0x06, makeMsg(0x07, 0x80, {}), needAck);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    svc.sendRaw(0x06, makeMsg(0x07, 0x81, {}), needAck);
    for (int i = 0; i < 5; i++) {
      std::vector<uint8_t> chunk(16, static_cast<uint8_t>(i));
      svc.sendRaw(0x06, makeMsg(0x07, 0x82, chunk), needAck);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    svc.sendRaw(0x06, makeMsg(0x07, 0x83, {}), needAck);
  }

  if (sendMsg) {
    std::vector<uint8_t> payload = makeMsg(static_cast<uint8_t>(msgType & 0xFF), static_cast<uint8_t>(msgCmd & 0xFF), msgData);
    svc.sendRaw(0x06, payload, needAck);
  }

  if (sendRaw) {
    if (rawPayload.empty()) {
      std::cerr << "--send-raw requires --payload-hex" << std::endl;
    } else {
      svc.sendRaw(static_cast<uint8_t>(rawFrameType & 0xFF), rawPayload, needAck);
    }
  }

  // 主循环：等待 Ctrl+C 退出
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  svc.deinit();
  std::cout << "stopped" << std::endl;
  return 0;
}
