#pragma once
#include <Arduino.h>

// Log levels
#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4
#define LOG_LEVEL_TRACE 5

#ifndef LOG_LEVEL
#define LOG_LEVEL LOG_LEVEL_INFO
#endif

// Internal helper to print with printf-like formatting
static inline void _log_printf(const char* fmt, ...) {
  va_list args; va_start(args, fmt);
  char buf[256];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
}

#if LOG_LEVEL >= LOG_LEVEL_ERROR
  #define LOGE(fmt, ...) _log_printf((String("[E] ") + fmt).c_str(), ##__VA_ARGS__)
#else
  #define LOGE(fmt, ...)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
  #define LOGW(fmt, ...) _log_printf((String("[W] ") + fmt).c_str(), ##__VA_ARGS__)
#else
  #define LOGW(fmt, ...)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
  #define LOGI(fmt, ...) _log_printf((String("[I] ") + fmt).c_str(), ##__VA_ARGS__)
#else
  #define LOGI(fmt, ...)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
  #define LOGD(fmt, ...) _log_printf((String("[D] ") + fmt).c_str(), ##__VA_ARGS__)
#else
  #define LOGD(fmt, ...)
#endif

#if LOG_LEVEL >= LOG_LEVEL_TRACE
  #define LOGT(fmt, ...) _log_printf((String("[T] ") + fmt).c_str(), ##__VA_ARGS__)
#else
  #define LOGT(fmt, ...)
#endif

