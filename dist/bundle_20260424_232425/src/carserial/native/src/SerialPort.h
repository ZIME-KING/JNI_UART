#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace carserial {

class SerialPort {
 public:
  SerialPort();
  ~SerialPort();

  bool open(const std::string& path, int baudrate);
  void close();
  bool isOpen() const;

  // read/write 是平台串口 API 的薄封装：
  // - Windows: ReadFile/WriteFile
  // - Linux/Android: ::read/::write
  int read(uint8_t* buf, int maxLen);
  int write(const uint8_t* buf, int len);

  // 获取最近一次平台错误码：
  // - Windows: GetLastError()
  // - Linux/Android: errno
  int lastErrno() const;

 private:
#if defined(_WIN32)
  void* handle_;
#endif
  int fd_;
  int last_errno_;
};

}  // namespace carserial

