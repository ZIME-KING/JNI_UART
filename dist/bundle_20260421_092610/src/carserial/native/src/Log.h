#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace carserial {

enum class LogLevel : int {
  Error = 0,
  Warn = 1,
  Info = 2,
  Debug = 3,
};

class Log {
 public:
  static void setRuntimeLevel(LogLevel level);
  static LogLevel runtimeLevel();
  static LogLevel runtimeLevelFromSystemProperty();
  static void write(LogLevel level, const char* tag, const char* fmt, ...);

 private:
  static void writeV(LogLevel level, const char* tag, const char* fmt, va_list ap);
};

}  // namespace carserial

