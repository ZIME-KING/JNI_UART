#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "Frame.h"
#include "LinkManager.h"

namespace carserial {

class CarSerialService {
 public:
  // 通用事件桥，供上层/JNI 使用。
  // 当前 eventId 定义：
  //   1 -> 链路状态（p1: 1 已连接 / 0 已断开）
  //   2 -> 原始帧（p1: seq, p2: frameType, p3: payloadLen, data: payload）
  //   3 -> 已解析消息（p1: seq, p2: frameType, p3: (msgType<<8)|cmd, data: 消息数据）
  using EventSink = std::function<void(int eventId, int p1, int p2, int p3, const std::string& s, const std::vector<uint8_t>& data)>;

  static CarSerialService& instance();

  bool init(const std::string& ttyPath);
  bool init(const std::string& ttyPath, int baudrate);
  void deinit();

  void setEventSink(EventSink sink);

  // 通过 LinkManager 发送一帧协议数据。
  // needAck=true 时走超时/重试等待 ACK 逻辑。
  int sendRaw(uint8_t frameType, const std::vector<uint8_t>& payload, bool needAck);

 private:
  CarSerialService() = default;

  void onFrame(const Frame& f);
  void onLinkState(bool connected);

  std::mutex mu_;
  LinkManager link_;
  EventSink sink_;
};

}  // namespace carserial

