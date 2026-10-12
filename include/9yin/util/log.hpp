#pragma once

#include <cstdio>
#include <string_view>

namespace nineyin::util {

enum class LogLevel { Debug, Info, Warn, Error };

void set_log_level(LogLevel level);
void log(LogLevel level, const char* fmt, ...);

#define NINEYIN_LOG_DEBUG(...) ::nineyin::util::log(::nineyin::util::LogLevel::Debug, __VA_ARGS__)
#define NINEYIN_LOG_INFO(...)  ::nineyin::util::log(::nineyin::util::LogLevel::Info,  __VA_ARGS__)
#define NINEYIN_LOG_WARN(...)  ::nineyin::util::log(::nineyin::util::LogLevel::Warn,  __VA_ARGS__)
#define NINEYIN_LOG_ERROR(...) ::nineyin::util::log(::nineyin::util::LogLevel::Error, __VA_ARGS__)

}  // namespace nineyin::util
