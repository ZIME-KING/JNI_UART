#pragma once

#include <cstdint>
#include <vector>

namespace carserial {

// 协议帧解码后的结构：
// - seq：帧序号，用于 ACK 匹配与重试。
// - frameType：帧类型（setup/ack/heartbeat/data 等）。
// - payload：frameType 后的原始负载；当前上层约定
//   payload[0]=MsgType，payload[1]=Cmd，payload[2..]=Data。
struct Frame {
  uint8_t seq = 0;
  uint8_t frameType = 0;
  std::vector<uint8_t> payload;
};

class FrameCodec {
 public:
  // 当前协议使用的校验算法：
  // 对 [len..payload] 求和，取低 8 位后按位取反。
  static uint8_t calcChecksum(const uint8_t* data, int len);
  // 编码为完整线协议帧：[FF AA][len][seq][frameType][payload][checksum]。
  static std::vector<uint8_t> encode(uint8_t seq, uint8_t frameType, const uint8_t* payload, int payloadLen);
  // 从 buf 解码一帧；格式错误或校验失败返回 false。
  static bool decode(const uint8_t* buf, int len, Frame* out);
};

}  // namespace carserial

