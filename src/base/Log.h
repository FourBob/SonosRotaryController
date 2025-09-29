#pragma once
#include <Arduino.h>

namespace sys {

enum class Level { Debug, Info, Warn, Error };

inline void logf(Level lvl, const char* tag, const char* fmt, ...) {
  static const char* L[] = {"D", "I", "W", "E"};
  va_list args; va_start(args, fmt);
  Serial.printf("[%s][%s] ", L[(int)lvl], tag ? tag : "-");
  Serial.vprintf(fmt, args);
  Serial.print('\n');
  va_end(args);
}

#define LOGD(TAG, FMT, ...) ::sys::logf(::sys::Level::Debug, TAG, FMT, ##__VA_ARGS__)
#define LOGI(TAG, FMT, ...) ::sys::logf(::sys::Level::Info,  TAG, FMT, ##__VA_ARGS__)
#define LOGW(TAG, FMT, ...) ::sys::logf(::sys::Level::Warn,  TAG, FMT, ##__VA_ARGS__)
#define LOGE(TAG, FMT, ...) ::sys::logf(::sys::Level::Error, TAG, FMT, ##__VA_ARGS__)

} // namespace sys

