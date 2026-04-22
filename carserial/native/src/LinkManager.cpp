#include "LinkManager.h"

#include <chrono>
#include <string>

#include "Log.h"

namespace carserial {

static constexpr uint8_t kFrameTypeAck = 0x02;
static constexpr uint8_t kFrameTypeHeartbeat = 0x05;
static constexpr uint8_t kFrameTypeSetup = 0x10;
static constexpr uint8_t kFrameTypeSetupAck = 0x11;

static std::string toHex(const uint8_t* data, int len, int limit) {
  if (!data || len <= 0) return "";
  if (limit <= 0) limit = 256;
  const int head = len <= limit ? len : limit / 2;
  const int tail = len <= limit ? 0 : limit - head;

  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(static_cast<size_t>(limit * 3 + 16));

  auto appendByte = [&](uint8_t b) {
    out.push_back(kHex[(b >> 4) & 0xF]);
    out.push_back(kHex[b & 0xF]);
  };

  for (int i = 0; i < head; i++) {
    if (i > 0) out.push_back(' ');
    appendByte(data[i]);
  }
  if (tail > 0) {
    out.append(" ... ");
    int start = len - tail;
    for (int i = start; i < len; i++) {
      if (i > start) out.push_back(' ');
      appendByte(data[i]);
    }
  }
  return out;
}

LinkManager::LinkManager() = default;

LinkManager::~LinkManager() {
  stop();
}

bool LinkManager::start(const std::string& ttyPath, int baudrate, FrameHandler frameHandler, LinkStateHandler linkHandler) {
  stop();

  ttyPath_ = ttyPath;
  baudrate_ = baudrate;
  frameHandler_ = std::move(frameHandler);
  linkHandler_ = std::move(linkHandler);

  running_.store(true, std::memory_order_relaxed);

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!port_.open(ttyPath_, baudrate_)) {
      running_.store(false, std::memory_order_relaxed);
      return false;
    }
    Log::setRuntimeLevel(Log::runtimeLevelFromSystemProperty());
    connected_.store(false, std::memory_order_relaxed);
    doSetupLocked();
  }

  reader_ = std::thread([this]() { readerLoop(); });
  heartbeat_ = std::thread([this]() { heartbeatLoop(); });
  return true;
}

void LinkManager::stop() {
  bool wasRunning = running_.exchange(false, std::memory_order_relaxed);
  if (!wasRunning) return;

  if (reader_.joinable()) reader_.join();
  if (heartbeat_.joinable()) heartbeat_.join();

  {
    std::lock_guard<std::mutex> lock(mu_);
    port_.close();
  }

  bool wasConnected = connected_.exchange(false, std::memory_order_relaxed);
  if (wasConnected && linkHandler_) linkHandler_(false);
}

uint8_t LinkManager::nextSeq() {
  uint8_t v = seq_.fetch_add(1, std::memory_order_relaxed);
  if (v == 0) v = seq_.fetch_add(1, std::memory_order_relaxed);
  return v;
}

bool LinkManager::ensureConnected() {
  if (connected_.load(std::memory_order_relaxed)) return true;
  std::lock_guard<std::mutex> lock(mu_);
  return doSetupLocked();
}

bool LinkManager::doSetupLocked() {
  if (!port_.isOpen()) return false;

  uint8_t seq = nextSeq();
  std::vector<uint8_t> bytes = FrameCodec::encode(seq, kFrameTypeSetup, nullptr, 0);
  Log::write(LogLevel::Debug, "carserial", "TX len=%d, hex: %s", static_cast<int>(bytes.size()),
             toHex(bytes.data(), static_cast<int>(bytes.size()), 256).c_str());
  int w = port_.write(bytes.data(), static_cast<int>(bytes.size()));
  if (w != static_cast<int>(bytes.size())) {
    return false;
  }
  return true;
}

int LinkManager::send(uint8_t frameType, const std::vector<uint8_t>& payload) {
  if (!ensureConnected()) return -1;

  uint8_t seq = nextSeq();
  std::vector<uint8_t> bytes = FrameCodec::encode(seq, frameType, payload.data(), static_cast<int>(payload.size()));
  std::lock_guard<std::mutex> lock(mu_);
  Log::write(LogLevel::Debug, "carserial", "TX len=%d, hex: %s", static_cast<int>(bytes.size()),
             toHex(bytes.data(), static_cast<int>(bytes.size()), 256).c_str());
  int w = port_.write(bytes.data(), static_cast<int>(bytes.size()));
  if (w != static_cast<int>(bytes.size())) return -1;
  return seq;
}

int LinkManager::sendWithAck(uint8_t frameType, const std::vector<uint8_t>& payload, int timeoutMs, int retryCount) {
  if (timeoutMs <= 0) timeoutMs = 200;
  if (retryCount <= 0) retryCount = 3;

  if (!ensureConnected()) return -1;

  for (int i = 0; i < retryCount; i++) {
    uint8_t seq = nextSeq();
    {
      std::vector<uint8_t> bytes = FrameCodec::encode(seq, frameType, payload.data(), static_cast<int>(payload.size()));
      std::lock_guard<std::mutex> lock(mu_);
      Log::write(LogLevel::Debug, "carserial", "TX len=%d, hex: %s", static_cast<int>(bytes.size()),
                 toHex(bytes.data(), static_cast<int>(bytes.size()), 256).c_str());
      int w = port_.write(bytes.data(), static_cast<int>(bytes.size()));
      if (w != static_cast<int>(bytes.size())) return -1;
    }

    std::unique_lock<std::mutex> lock(mu_);
    bool ok = ackCv_.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
      return acked_.find(static_cast<int>(seq)) != acked_.end();
    });
    if (ok) {
      acked_.erase(static_cast<int>(seq));
      return seq;
    }
    Log::write(LogLevel::Warn, "carserial", "ACK timeout seq=%u retry=%d/%d", static_cast<unsigned>(seq), i + 1,
               retryCount);
  }
  return -1;
}

void LinkManager::readerLoop() {
  uint8_t tmp[2048];
  while (running_.load(std::memory_order_relaxed)) {
    int r = -1;
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (port_.isOpen()) r = port_.read(tmp, static_cast<int>(sizeof(tmp)));
    }
    if (r < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }
    if (r == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      continue;
    }

    Log::write(LogLevel::Debug, "carserial", "RX len=%d, hex: %s", r, toHex(tmp, r, 256).c_str());
    parser_.push(tmp, r);
    Frame f;
    while (parser_.pop(&f)) onFrame(f);
  }
}

void LinkManager::heartbeatLoop() {
  while (running_.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    if (!running_.load(std::memory_order_relaxed)) break;
    if (!ensureConnected()) continue;
    sendWithAck(kFrameTypeHeartbeat, {}, 200, 1);
  }
}

void LinkManager::onFrame(const Frame& f) {
  if (f.frameType == kFrameTypeAck) {
    std::lock_guard<std::mutex> lock(mu_);
    acked_.insert(static_cast<int>(f.seq));
    ackCv_.notify_all();
    return;
  }
  uint8_t msgType = 0;
  uint8_t cmd = 0;
  int dataLen = static_cast<int>(f.payload.size());
  if (dataLen >= 2) {
    msgType = f.payload[0];
    cmd = f.payload[1];
    dataLen -= 2;
  }
  Log::write(LogLevel::Debug, "carserial",
             "RX frame: type=0x%02X msgType=0x%02X cmd=0x%02X seq=%u dataLen=%d checksum=OK", f.frameType, msgType,
             cmd, static_cast<unsigned>(f.seq), dataLen);
  if (f.frameType == kFrameTypeSetupAck) {
    bool was = connected_.exchange(true, std::memory_order_relaxed);
    if (!was && linkHandler_) linkHandler_(true);
  }
  if (frameHandler_) frameHandler_(f);
}

}  // namespace carserial

