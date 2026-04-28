#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "CarSerialService.h"
#include "Frame.h"

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

static uint64_t nowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static std::string toHex(const std::vector<uint8_t>& data) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  if (data.empty()) return out;
  out.reserve(data.size() * 3);
  for (size_t i = 0; i < data.size(); i++) {
    uint8_t b = data[i];
    out.push_back(kHex[(b >> 4) & 0x0F]);
    out.push_back(kHex[b & 0x0F]);
    if (i + 1 < data.size()) out.push_back(' ');
  }
  return out;
}

static void writeJsonString(std::ostream& os, const std::string& s) {
  os << '"';
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '\\') {
      os << "\\\\";
    } else if (c == '"') {
      os << "\\\"";
    } else if (c == '\n') {
      os << "\\n";
    } else if (c == '\r') {
      os << "\\r";
    } else if (c == '\t') {
      os << "\\t";
    } else if (c < 0x20) {
      os << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec;
    } else {
      os << static_cast<char>(c);
    }
  }
  os << '"';
}

static std::vector<std::string> splitTokens(const std::string& line) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < line.size(); i++) {
    unsigned char ch = static_cast<unsigned char>(line[i]);
    if (std::isspace(ch)) {
      if (!cur.empty()) {
        out.push_back(cur);
        cur.clear();
      }
      continue;
    }
    cur.push_back(static_cast<char>(ch));
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

static void usage() {
  // 命令行参数说明见 usage 输出；Windows 里 Ctrl+C 退出。
  std::cout << "Usage:\n"
            << "  carserial_host_tool <PORT> [--baud <baud>]\n"
            << "    [--send-msg <msgType> <cmd> [--data-hex <hex>] [--data-ascii <text>] [--need-ack <0|1>]]\n"
            << "    [--send-raw <frameType> --payload-hex <hex> [--need-ack <0|1>]]\n"
            << "    [--send-radio-freq <band> <freq>]\n"
            << "    [--update-demo]\n"
            << "    [--replay <frames.txt> [--delay-ms <ms>] [--tail-ms <ms>] [--log <path>] [--echo <0|1>]]\n"
            << "\n"
            << "Examples:\n"
            << "  carserial_host_tool COM5 --baud 460800\n"
            << "  carserial_host_tool COM5 --send-msg 0x01 0x80 --data-hex \"01 02 03\" --need-ack 1\n"
            << "  carserial_host_tool COM5 --send-raw 0x06 --payload-hex \"01 0A 00 64\" --need-ack 1\n"
            << "  carserial_host_tool COM5 --send-radio-freq 0 10170\n"
            << "  carserial_host_tool COM5 --update-demo\n"
            << "  carserial_host_tool /dev/ttyS1 --replay /data/local/tmp/frames.txt --log /data/local/tmp/serial_log.jsonl\n";
}
}  // namespace

int main(int argc, char** argv) {
#ifndef CAR_SERIAL_DEFAULT_TTY
#define CAR_SERIAL_DEFAULT_TTY ""
#endif

  std::string tty = (std::string(CAR_SERIAL_DEFAULT_TTY).empty() ? "COM5" : CAR_SERIAL_DEFAULT_TTY);
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
  std::string replayPath;
  std::string logPath;
  int delayMs = 50;
  int tailMs = 500;
  bool echo = true;

  int optStart = 1;
  if (argc >= 2 && argv[1]) {
    std::string a1 = argv[1];
    if (!a1.empty() && a1[0] != '-') {
      tty = a1;
      optStart = 2;
    }
  }
  // 解析可选参数
  for (int i = optStart; i < argc; i++) {
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
    if (std::strcmp(a, "--delay-ms") == 0 && i + 1 < argc) {
      delayMs = std::atoi(argv[++i]);
      if (delayMs < 0) delayMs = 0;
      continue;
    }
    if (std::strcmp(a, "--tail-ms") == 0 && i + 1 < argc) {
      tailMs = std::atoi(argv[++i]);
      if (tailMs < 0) tailMs = 0;
      continue;
    }
    if (std::strcmp(a, "--echo") == 0 && i + 1 < argc) {
      int v = std::atoi(argv[++i]);
      echo = v != 0;
      continue;
    }
    if (std::strcmp(a, "--log") == 0 && i + 1 < argc) {
      logPath = argv[++i];
      continue;
    }
    if (std::strcmp(a, "--replay") == 0 && i + 1 < argc) {
      replayPath = argv[++i];
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
  std::ofstream logFile;
  if (!logPath.empty()) {
    logFile.open(logPath, std::ios::out | std::ios::trunc);
  } else if (!replayPath.empty()) {
    logFile.open("carserial_log.jsonl", std::ios::out | std::ios::trunc);
  }

  auto logJson = [&](const std::string& line) {
    if (logFile.is_open()) {
      logFile << line << "\n";
      logFile.flush();
    }
  };

  auto logTxFrame = [&](int seq, uint8_t frameType, const std::vector<uint8_t>& payload) {
    if (!logFile.is_open()) return;
    std::vector<uint8_t> raw = seq < 0 ? std::vector<uint8_t>() : carserial::FrameCodec::encode(static_cast<uint8_t>(seq & 0xFF), frameType,
                                                                                                 payload.empty() ? nullptr : payload.data(),
                                                                                                 static_cast<int>(payload.size()));
    std::ostringstream os;
    os << "{\"tsMs\":" << nowMs() << ",\"dir\":\"TX\",\"kind\":\"frame\",\"seq\":" << seq << ",\"frameType\":" << static_cast<int>(frameType)
       << ",\"payloadHex\":";
    writeJsonString(os, toHex(payload));
    os << ",\"rawHex\":";
    writeJsonString(os, toHex(raw));
    os << "}";
    logJson(os.str());
  };

  svc.setEventSink([&](int eventId, int p1, int p2, int p3, const std::string&, const std::vector<uint8_t>& data) {
    // 事件定义见 CarSerialService.h：
    // 1=链路状态，2=原始帧，3=解析后的 MsgType/Cmd
    if (eventId == 1) {
      std::cout << "[link] connected=" << (p1 == 1 ? "true" : "false") << std::endl;
      if (logFile.is_open()) {
        std::ostringstream os;
        os << "{\"tsMs\":" << nowMs() << ",\"kind\":\"link\",\"connected\":" << (p1 == 1 ? "true" : "false") << "}";
        logJson(os.str());
      }
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
      if (logFile.is_open()) {
        std::ostringstream os;
        os << "{\"tsMs\":" << nowMs() << ",\"dir\":\"RX\",\"kind\":\"msg\",\"seq\":" << p1 << ",\"frameType\":"
           << p2 << ",\"msgType\":" << msgType << ",\"cmd\":" << cmd << ",\"dataHex\":";
        writeJsonString(os, toHex(data));
        os << "}";
        logJson(os.str());
      }
      return;
    }
    if (eventId == 2) {
      // 原始帧打印：这里只打印长度信息；更详细的 TX/RX hex 在 LinkManager.cpp 里按日志级别输出
      std::cout << "[raw] seq=" << p1 << " frameType=0x" << std::hex << p2 << std::dec << " payloadLen=" << p3
                << std::endl;
      if (logFile.is_open()) {
        std::vector<uint8_t> raw = carserial::FrameCodec::encode(static_cast<uint8_t>(p1 & 0xFF), static_cast<uint8_t>(p2 & 0xFF),
                                                                 data.empty() ? nullptr : data.data(), static_cast<int>(data.size()));
        std::ostringstream os;
        os << "{\"tsMs\":" << nowMs() << ",\"dir\":\"RX\",\"kind\":\"frame\",\"seq\":" << p1 << ",\"frameType\":" << p2
           << ",\"payloadHex\":";
        writeJsonString(os, toHex(data));
        os << ",\"rawHex\":";
        writeJsonString(os, toHex(raw));
        os << "}";
        logJson(os.str());
      }
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
    int seq = svc.sendRaw(0x06, payload, needAck);
    logTxFrame(seq, 0x06, payload);
  }

  if (updateDemo) {
    // 发送一套升级流程演示：
    // - 0x80 req
    // - 0x81 begin
    // - 0x82 data（这里发送 5 次小数据块）
    // - 0x83 end
    {
      std::vector<uint8_t> payload = makeMsg(0x07, 0x80, {});
      int seq = svc.sendRaw(0x06, payload, needAck);
      logTxFrame(seq, 0x06, payload);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    {
      std::vector<uint8_t> payload = makeMsg(0x07, 0x81, {});
      int seq = svc.sendRaw(0x06, payload, needAck);
      logTxFrame(seq, 0x06, payload);
    }
    for (int i = 0; i < 5; i++) {
      std::vector<uint8_t> chunk(16, static_cast<uint8_t>(i));
      std::vector<uint8_t> payload = makeMsg(0x07, 0x82, chunk);
      int seq = svc.sendRaw(0x06, payload, needAck);
      logTxFrame(seq, 0x06, payload);
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
      std::vector<uint8_t> payload = makeMsg(0x07, 0x83, {});
      int seq = svc.sendRaw(0x06, payload, needAck);
      logTxFrame(seq, 0x06, payload);
    }
  }

  if (sendMsg) {
    std::vector<uint8_t> payload = makeMsg(static_cast<uint8_t>(msgType & 0xFF), static_cast<uint8_t>(msgCmd & 0xFF), msgData);
    int seq = svc.sendRaw(0x06, payload, needAck);
    logTxFrame(seq, 0x06, payload);
  }

  if (sendRaw) {
    if (rawPayload.empty()) {
      std::cerr << "--send-raw requires --payload-hex" << std::endl;
    } else {
      uint8_t ft = static_cast<uint8_t>(rawFrameType & 0xFF);
      int seq = svc.sendRaw(ft, rawPayload, needAck);
      logTxFrame(seq, ft, rawPayload);
    }
  }

  if (!replayPath.empty()) {
    std::ifstream in(replayPath, std::ios::in);
    if (!in.is_open()) {
      std::cerr << "failed to open replay file: " << replayPath << std::endl;
      svc.deinit();
      return 2;
    }

    std::string line;
    int lineNo = 0;
    while (std::getline(in, line) && !g_stop) {
      lineNo++;
      std::string trimmed = line;
      while (!trimmed.empty() && (trimmed.back() == '\r' || trimmed.back() == '\n' || trimmed.back() == ' ' || trimmed.back() == '\t')) {
        trimmed.pop_back();
      }
      size_t start = 0;
      while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\t')) start++;
      if (start > 0) trimmed.erase(0, start);
      if (trimmed.empty()) continue;
      if (trimmed.rfind("#", 0) == 0 || trimmed.rfind("//", 0) == 0) continue;

      std::vector<std::string> tok = splitTokens(trimmed);
      if (tok.empty()) continue;

      bool ok = true;
      uint8_t ft = 0x06;
      std::vector<uint8_t> payload;

      if (tok[0] == "msg" || tok[0] == "MSG") {
        if (tok.size() < 3) {
          std::cerr << "invalid msg line: " << lineNo << std::endl;
          ok = false;
        } else {
          int mt = 0;
          int cmd = 0;
          if (!parseInt(tok[1], &mt) || !parseInt(tok[2], &cmd)) ok = false;
          std::vector<uint8_t> dataBytes;
          if (tok.size() > 3) {
            std::string rest;
            for (size_t i = 3; i < tok.size(); i++) {
              if (!rest.empty()) rest.push_back(' ');
              rest += tok[i];
            }
            ok = parseHexBytes(rest, &dataBytes);
          }
          payload = makeMsg(static_cast<uint8_t>(mt & 0xFF), static_cast<uint8_t>(cmd & 0xFF), dataBytes);
          ft = 0x06;
        }
      } else if (tok[0] == "frame" || tok[0] == "FRAME") {
        if (tok.size() < 2) {
          std::cerr << "invalid frame line: " << lineNo << std::endl;
          ok = false;
        } else {
          int frameType = 0;
          size_t payloadIdx = 2;
          if (tok.size() >= 3) {
            int ignoreSeq = 0;
            if (!parseInt(tok[1], &ignoreSeq) || !parseInt(tok[2], &frameType)) ok = false;
            payloadIdx = 3;
          } else {
            ok = parseInt(tok[1], &frameType);
          }
          ft = static_cast<uint8_t>(frameType & 0xFF);
          if (ok && tok.size() > payloadIdx) {
            std::string rest;
            for (size_t i = payloadIdx; i < tok.size(); i++) {
              if (!rest.empty()) rest.push_back(' ');
              rest += tok[i];
            }
            ok = parseHexBytes(rest, &payload);
          }
        }
      } else if (tok[0] == "raw" || tok[0] == "RAW" || trimmed.rfind("FF", 0) == 0 || trimmed.rfind("ff", 0) == 0) {
        std::string rest = trimmed;
        if (tok[0] == "raw" || tok[0] == "RAW") {
          rest.clear();
          for (size_t i = 1; i < tok.size(); i++) {
            if (!rest.empty()) rest.push_back(' ');
            rest += tok[i];
          }
        }
        std::vector<uint8_t> rawBytes;
        ok = parseHexBytes(rest, &rawBytes);
        if (ok) {
          carserial::Frame decoded;
          ok = carserial::FrameCodec::decode(rawBytes.data(), static_cast<int>(rawBytes.size()), &decoded);
          if (ok) {
            ft = decoded.frameType;
            payload = decoded.payload;
          }
        }
      } else {
        std::cerr << "unknown line prefix: " << tok[0] << " at line " << lineNo << std::endl;
        ok = false;
      }

      if (!ok) {
        if (logFile.is_open()) {
          std::ostringstream os;
          os << "{\"tsMs\":" << nowMs() << ",\"kind\":\"parse_error\",\"line\":" << lineNo << ",\"text\":";
          writeJsonString(os, trimmed);
          os << "}";
          logJson(os.str());
        }
        continue;
      }

      int seq = svc.sendRaw(ft, payload, needAck);
      if (logFile.is_open()) {
        std::vector<uint8_t> raw = seq < 0 ? std::vector<uint8_t>() : carserial::FrameCodec::encode(static_cast<uint8_t>(seq & 0xFF), ft,
                                                                                                     payload.empty() ? nullptr : payload.data(),
                                                                                                     static_cast<int>(payload.size()));
        std::ostringstream os;
        os << "{\"tsMs\":" << nowMs() << ",\"dir\":\"TX\",\"kind\":\"frame\",\"seq\":" << seq << ",\"frameType\":"
           << static_cast<int>(ft) << ",\"payloadHex\":";
        writeJsonString(os, toHex(payload));
        os << ",\"rawHex\":";
        writeJsonString(os, toHex(raw));
        os << ",\"line\":" << lineNo << "}";
        logJson(os.str());
      }

      if (echo) {
        std::cout << "[tx] line=" << lineNo << " seq=" << seq << " frameType=0x" << std::hex << static_cast<int>(ft) << std::dec
                  << " payloadLen=" << payload.size() << std::endl;
      }
      if (delayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }

    if (tailMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(tailMs));
    svc.deinit();
    if (logFile.is_open()) std::cout << "saved log: " << (logPath.empty() ? "carserial_log.jsonl" : logPath) << std::endl;
    return 0;
  }

  // 主循环：等待 Ctrl+C 退出
  while (!g_stop) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  svc.deinit();
  std::cout << "stopped" << std::endl;
  return 0;
}
