#include "SerialPort.h"

#include <cerrno>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#if defined(__ANDROID__) || defined(__linux__)
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace carserial {

SerialPort::SerialPort()
#if defined(_WIN32)
    : handle_(INVALID_HANDLE_VALUE), fd_(-1), last_errno_(0) {}
#else
    : fd_(-1), last_errno_(0) {}
#endif

SerialPort::~SerialPort() {
  close();
}

bool SerialPort::open(const std::string& path, int baudrate) {
  close();

#if defined(_WIN32)
  std::string device = path;
  if (device.rfind("\\\\.\\", 0) != 0) {
    device = "\\\\.\\" + device;
  }
  handle_ = CreateFileA(device.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  if (handle_ == INVALID_HANDLE_VALUE) {
    last_errno_ = static_cast<int>(GetLastError());
    return false;
  }

  DCB dcb{};
  dcb.DCBlength = sizeof(dcb);
  if (!GetCommState(static_cast<HANDLE>(handle_), &dcb)) {
    last_errno_ = static_cast<int>(GetLastError());
    close();
    return false;
  }
  dcb.BaudRate = static_cast<DWORD>(baudrate);
  dcb.ByteSize = 8;
  dcb.Parity = NOPARITY;
  dcb.StopBits = ONESTOPBIT;
  dcb.fBinary = TRUE;
  dcb.fDtrControl = DTR_CONTROL_DISABLE;
  dcb.fRtsControl = RTS_CONTROL_DISABLE;
  if (!SetCommState(static_cast<HANDLE>(handle_), &dcb)) {
    last_errno_ = static_cast<int>(GetLastError());
    close();
    return false;
  }

  COMMTIMEOUTS to{};
  to.ReadIntervalTimeout = 20;
  to.ReadTotalTimeoutConstant = 20;
  to.ReadTotalTimeoutMultiplier = 0;
  to.WriteTotalTimeoutConstant = 50;
  to.WriteTotalTimeoutMultiplier = 0;
  if (!SetCommTimeouts(static_cast<HANDLE>(handle_), &to)) {
    last_errno_ = static_cast<int>(GetLastError());
    close();
    return false;
  }

  last_errno_ = 0;
  return true;
#elif defined(__ANDROID__) || defined(__linux__)
  fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
  if (fd_ < 0) {
    last_errno_ = errno;
    return false;
  }

  termios tty{};
  if (tcgetattr(fd_, &tty) != 0) {
    last_errno_ = errno;
    close();
    return false;
  }

  cfmakeraw(&tty);
  tty.c_cflag |= (CLOCAL | CREAD);
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~PARENB;
  tty.c_cflag &= ~CSIZE;
  tty.c_cflag |= CS8;
  tty.c_cc[VMIN] = 0;
  tty.c_cc[VTIME] = 1;

  speed_t speed = B115200;
  switch (baudrate) {
    case 460800:
#ifdef B460800
      speed = B460800;
#else
      speed = B115200;
#endif
      break;
    case 115200:
      speed = B115200;
      break;
    default:
      speed = B115200;
      break;
  }

  if (cfsetispeed(&tty, speed) != 0 || cfsetospeed(&tty, speed) != 0) {
    last_errno_ = errno;
    close();
    return false;
  }

  if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
    last_errno_ = errno;
    close();
    return false;
  }

  last_errno_ = 0;
  return true;
#else
  (void)path;
  (void)baudrate;
  last_errno_ = 0;
  return false;
#endif
}

void SerialPort::close() {
#if defined(_WIN32)
  if (handle_ != INVALID_HANDLE_VALUE) {
    CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = INVALID_HANDLE_VALUE;
  }
#elif defined(__ANDROID__) || defined(__linux__)
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
#endif
}

bool SerialPort::isOpen() const {
#if defined(_WIN32)
  return handle_ != INVALID_HANDLE_VALUE;
#else
  return fd_ >= 0;
#endif
}

int SerialPort::read(uint8_t* buf, int maxLen) {
#if defined(_WIN32)
  if (handle_ == INVALID_HANDLE_VALUE) return -1;
  DWORD n = 0;
  if (!ReadFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(maxLen), &n, nullptr)) {
    last_errno_ = static_cast<int>(GetLastError());
    return -1;
  }
  return static_cast<int>(n);
#elif defined(__ANDROID__) || defined(__linux__)
  if (fd_ < 0) return -1;
  int r = static_cast<int>(::read(fd_, buf, static_cast<size_t>(maxLen)));
  if (r < 0) last_errno_ = errno;
  return r;
#else
  (void)buf;
  (void)maxLen;
  return -1;
#endif
}

int SerialPort::write(const uint8_t* buf, int len) {
#if defined(_WIN32)
  if (handle_ == INVALID_HANDLE_VALUE) return -1;
  DWORD n = 0;
  if (!WriteFile(static_cast<HANDLE>(handle_), buf, static_cast<DWORD>(len), &n, nullptr)) {
    last_errno_ = static_cast<int>(GetLastError());
    return -1;
  }
  return static_cast<int>(n);
#elif defined(__ANDROID__) || defined(__linux__)
  if (fd_ < 0) return -1;
  int w = static_cast<int>(::write(fd_, buf, static_cast<size_t>(len)));
  if (w < 0) last_errno_ = errno;
  return w;
#else
  (void)buf;
  (void)len;
  return -1;
#endif
}

int SerialPort::lastErrno() const {
  return last_errno_;
}

}  // namespace carserial

