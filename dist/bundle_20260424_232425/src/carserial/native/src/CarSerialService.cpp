#include "CarSerialService.h"

#include "Log.h"

namespace carserial {

static constexpr int kEventLinkState = 1;
static constexpr int kEventRawFrame = 2;
static constexpr int kEventMessage = 3;

CarSerialService& CarSerialService::instance() {
  static CarSerialService inst;
  return inst;
}

bool CarSerialService::init(const std::string& ttyPath) {
  return init(ttyPath, 460800);
}

bool CarSerialService::init(const std::string& ttyPath, int baudrate) {
  std::lock_guard<std::mutex> lock(mu_);
  Log::setRuntimeLevel(Log::runtimeLevelFromSystemProperty());
  return link_.start(
      ttyPath,
      baudrate,
      [this](const Frame& f) { onFrame(f); },
      [this](bool c) { onLinkState(c); });
}

void CarSerialService::deinit() {
  std::lock_guard<std::mutex> lock(mu_);
  link_.stop();
  sink_ = nullptr;
}

void CarSerialService::setEventSink(EventSink sink) {
  std::lock_guard<std::mutex> lock(mu_);
  sink_ = std::move(sink);
}

int CarSerialService::sendRaw(uint8_t frameType, const std::vector<uint8_t>& payload, bool needAck) {
  if (needAck) return link_.sendWithAck(frameType, payload, 200, 3);
  return link_.send(frameType, payload);
}

void CarSerialService::onFrame(const Frame& f) {
  EventSink sinkCopy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sinkCopy = sink_;
  }
  if (!sinkCopy) return;

  sinkCopy(kEventRawFrame, f.seq, f.frameType, static_cast<int>(f.payload.size()), "", f.payload);

  if (f.payload.size() < 2) return;
  int packed = (static_cast<int>(f.payload[0]) << 8) | static_cast<int>(f.payload[1]);
  std::vector<uint8_t> data;
  if (f.payload.size() > 2) data.assign(f.payload.begin() + 2, f.payload.end());
  sinkCopy(kEventMessage, f.seq, f.frameType, packed, "", data);
}

void CarSerialService::onLinkState(bool connected) {
  EventSink sinkCopy;
  {
    std::lock_guard<std::mutex> lock(mu_);
    sinkCopy = sink_;
  }
  if (sinkCopy) sinkCopy(kEventLinkState, connected ? 1 : 0, 0, 0, "", {});
}

}  // namespace carserial
