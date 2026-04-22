#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "Frame.h"
#include "FrameStreamParser.h"
#include "SerialPort.h"

namespace carserial {

class LinkManager {
 public:
  // frameHandler：收到并解码非链路控制帧后回调。
  // linkHandler：链路状态变化时回调（connected/disconnected）。
  using FrameHandler = std::function<void(const Frame&)>;
  using LinkStateHandler = std::function<void(bool connected)>;

  LinkManager();
  ~LinkManager();

  bool start(const std::string& ttyPath, int baudrate, FrameHandler frameHandler, LinkStateHandler linkHandler);
  void stop();

  // send：直接发送，不等待 ACK。
  // sendWithAck：发送后按 seq 等待 ACK（超时与重试次数由调用方指定）。
  int send(uint8_t frameType, const std::vector<uint8_t>& payload);
  int sendWithAck(uint8_t frameType, const std::vector<uint8_t>& payload, int timeoutMs, int retryCount);

 private:
  uint8_t nextSeq();
  bool ensureConnected();
  bool doSetupLocked();
  void readerLoop();
  void heartbeatLoop();
  void onFrame(const Frame& f);

  std::mutex mu_;
  SerialPort port_;
  std::string ttyPath_;
  int baudrate_ = 460800;
  std::atomic<bool> running_{false};
  std::atomic<bool> connected_{false};
  std::thread reader_;
  std::thread heartbeat_;

  FrameStreamParser parser_;
  FrameHandler frameHandler_;
  LinkStateHandler linkHandler_;

  std::atomic<uint8_t> seq_{0};
  // acked_ 保存对端已确认的 seq。
  // sendWithAck 在 ackCv_ 上等待，直到目标 seq 出现在该集合中。
  std::condition_variable ackCv_;
  std::unordered_set<int> acked_;
};

}  // namespace carserial

