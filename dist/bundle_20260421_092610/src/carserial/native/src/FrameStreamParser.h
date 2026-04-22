#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "Frame.h"

namespace carserial {

class FrameStreamParser {
 public:
  void push(const uint8_t* data, int len);
  bool pop(Frame* out);

 private:
  std::deque<uint8_t> buf_;
};

}  // namespace carserial

