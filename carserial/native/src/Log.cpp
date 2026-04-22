#include "Log.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif

namespace carserial {

static std::atomic<int> g_runtime_level(static_cast<int>(LogLevel::Info));

static bool allows(LogLevel level) {
#if !defined(__ANDROID__)
  (void)level;
  return true;
#elif defined(CAR_SERIAL_LOG_LEVEL_DEBUG)
  (void)level;
  return true;
#elif defined(CAR_SERIAL_LOG_LEVEL_INFO)
  return static_cast<int>(level) <= static_cast<int>(LogLevel::Info);
#elif defined(CAR_SERIAL_LOG_LEVEL_WARN)
  return static_cast<int>(level) <= static_cast<int>(LogLevel::Warn);
#elif defined(CAR_SERIAL_LOG_LEVEL_ERROR)
  return static_cast<int>(level) <= static_cast<int>(LogLevel::Error);
#else
  return static_cast<int>(level) <= static_cast<int>(LogLevel::Info);
#endif
}

void Log::setRuntimeLevel(LogLevel level) {
  g_runtime_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel Log::runtimeLevel() {
  return static_cast<LogLevel>(g_runtime_level.load(std::memory_order_relaxed));
}

LogLevel Log::runtimeLevelFromSystemProperty() {
#if defined(__ANDROID__)
  char value[PROP_VALUE_MAX]{0};
  int len = __system_property_get("persist.carserial.log.level", value);
  if (len <= 0) {
    return LogLevel::Info;
  }
  if (std::strcmp(value, "DEBUG") == 0) return LogLevel::Debug;
  if (std::strcmp(value, "INFO") == 0) return LogLevel::Info;
  if (std::strcmp(value, "WARN") == 0) return LogLevel::Warn;
  if (std::strcmp(value, "ERROR") == 0) return LogLevel::Error;
  return LogLevel::Info;
#else
  const char* value = std::getenv("CAR_SERIAL_LOG_LEVEL");
  if (!value || !*value) return LogLevel::Debug;
  if (std::strcmp(value, "DEBUG") == 0) return LogLevel::Debug;
  if (std::strcmp(value, "INFO") == 0) return LogLevel::Info;
  if (std::strcmp(value, "WARN") == 0) return LogLevel::Warn;
  if (std::strcmp(value, "ERROR") == 0) return LogLevel::Error;
  return LogLevel::Debug;
#endif
}

void Log::write(LogLevel level, const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  writeV(level, tag, fmt, ap);
  va_end(ap);
}

void Log::writeV(LogLevel level, const char* tag, const char* fmt, va_list ap) {
  if (!allows(level)) return;
  if (static_cast<int>(level) > static_cast<int>(runtimeLevel())) return;

#if defined(__ANDROID__)
  int prio = ANDROID_LOG_INFO;
  switch (level) {
    case LogLevel::Error:
      prio = ANDROID_LOG_ERROR;
      break;
    case LogLevel::Warn:
      prio = ANDROID_LOG_WARN;
      break;
    case LogLevel::Info:
      prio = ANDROID_LOG_INFO;
      break;
    case LogLevel::Debug:
      prio = ANDROID_LOG_DEBUG;
      break;
  }
  __android_log_vprint(prio, tag ? tag : "carserial", fmt, ap);
#else
  const char* levelText = "INFO";
  switch (level) {
    case LogLevel::Error:
      levelText = "ERROR";
      break;
    case LogLevel::Warn:
      levelText = "WARN";
      break;
    case LogLevel::Info:
      levelText = "INFO";
      break;
    case LogLevel::Debug:
      levelText = "DEBUG";
      break;
  }
  std::fprintf(stderr, "[%s][%s] ", tag ? tag : "carserial", levelText);
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");
  std::fflush(stderr);
#endif
}

}  // namespace carserial

