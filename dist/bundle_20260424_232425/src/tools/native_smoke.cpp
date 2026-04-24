#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

#include "Frame.h"
#include "FrameStreamParser.h"

int main() {
  using namespace carserial;

  // payload 布局：[MsgType, Cmd, P0...Pn]
  const uint8_t payload[] = {0x01, 0x0A, 0x12, 0x34, 0x64};

  // 先编码再解码，验证 FrameCodec 编解码一致性。
  std::vector<uint8_t> encoded = FrameCodec::encode(0x21, 0x06, payload, static_cast<int>(sizeof(payload)));
  Frame decoded;
  bool ok = FrameCodec::decode(encoded.data(), static_cast<int>(encoded.size()), &decoded);
  assert(ok);
  assert(decoded.seq == 0x21);
  assert(decoded.frameType == 0x06);
  assert(decoded.payload.size() == sizeof(payload));
  assert(decoded.payload[0] == 0x01);
  assert(decoded.payload[1] == 0x0A);

  // 验证 FrameStreamParser 支持分包（把一帧拆成两段喂入）。
  FrameStreamParser parser;
  parser.push(encoded.data(), 3);
  parser.push(encoded.data() + 3, static_cast<int>(encoded.size()) - 3);
  Frame out;
  bool popped = parser.pop(&out);
  assert(popped);
  assert(out.seq == decoded.seq);
  assert(out.frameType == decoded.frameType);
  assert(out.payload == decoded.payload);

  std::cout << "native_smoke OK" << std::endl;
  return 0;
}
