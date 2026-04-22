#include "FrameStreamParser.h"

namespace carserial {

static constexpr uint8_t kHeader0 = 0xFF;
static constexpr uint8_t kHeader1 = 0xAA;

void FrameStreamParser::push(const uint8_t* data, int len) {
  if (!data || len <= 0) return;
  for (int i = 0; i < len; i++) buf_.push_back(data[i]);
}

bool FrameStreamParser::pop(Frame* out) {
  if (!out) return false;

  while (buf_.size() >= 7) {
    while (buf_.size() >= 2 && !(buf_[0] == kHeader0 && buf_[1] == kHeader1)) {
      buf_.pop_front();
    }
    if (buf_.size() < 7) return false;

    uint16_t totalLen = static_cast<uint16_t>((static_cast<uint16_t>(buf_[2]) << 8) | buf_[3]);
    if (totalLen < 7) {
      buf_.pop_front();
      continue;
    }
    if (buf_.size() < totalLen) return false;

    std::vector<uint8_t> frameBytes;
    frameBytes.reserve(totalLen);
    for (uint16_t i = 0; i < totalLen; i++) frameBytes.push_back(buf_[i]);

    Frame decoded;
    if (!FrameCodec::decode(frameBytes.data(), static_cast<int>(frameBytes.size()), &decoded)) {
      buf_.pop_front();
      continue;
    }

    for (uint16_t i = 0; i < totalLen; i++) buf_.pop_front();
    *out = std::move(decoded);
    return true;
  }
  return false;
}

}  // namespace carserial

