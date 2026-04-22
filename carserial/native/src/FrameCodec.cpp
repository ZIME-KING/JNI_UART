#include "Frame.h"

#include <cstring>

namespace carserial {

static constexpr uint8_t kHeader0 = 0xFF;
static constexpr uint8_t kHeader1 = 0xAA;

uint8_t FrameCodec::calcChecksum(const uint8_t* data, int len) {
  uint32_t sum = 0;
  for (int i = 0; i < len; i++) sum += data[i];
  uint8_t s = static_cast<uint8_t>(sum & 0xFF);
  return static_cast<uint8_t>(~s);
}

std::vector<uint8_t> FrameCodec::encode(uint8_t seq, uint8_t frameType, const uint8_t* payload, int payloadLen) {
  const int headerLen = 2;
  const int lenLen = 2;
  const int fixedLen = headerLen + lenLen + 1 + 1 + 1;
  const uint16_t totalLen = static_cast<uint16_t>(fixedLen + payloadLen);

  std::vector<uint8_t> out;
  out.resize(totalLen);
  out[0] = kHeader0;
  out[1] = kHeader1;
  out[2] = static_cast<uint8_t>((totalLen >> 8) & 0xFF);
  out[3] = static_cast<uint8_t>(totalLen & 0xFF);
  out[4] = seq;
  out[5] = frameType;
  if (payloadLen > 0) {
    std::memcpy(&out[6], payload, static_cast<size_t>(payloadLen));
  }
  uint8_t cs = calcChecksum(&out[2], (totalLen - 2) - 1);
  out[totalLen - 1] = cs;
  return out;
}

bool FrameCodec::decode(const uint8_t* buf, int len, Frame* out) {
  if (!buf || len < 7 || !out) return false;
  if (buf[0] != kHeader0 || buf[1] != kHeader1) return false;
  uint16_t totalLen = static_cast<uint16_t>((buf[2] << 8) | buf[3]);
  if (totalLen < 7) return false;
  if (totalLen > static_cast<uint16_t>(len)) return false;

  uint8_t expect = calcChecksum(&buf[2], (totalLen - 2) - 1);
  uint8_t got = buf[totalLen - 1];
  if (expect != got) return false;

  out->seq = buf[4];
  out->frameType = buf[5];
  int payloadLen = static_cast<int>(totalLen) - 7;
  out->payload.assign(&buf[6], &buf[6 + payloadLen]);
  return true;
}

}  // namespace carserial

