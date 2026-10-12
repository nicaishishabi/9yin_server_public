#include "9yin/util/log.hpp"

#include <cstdarg>
#include <ctime>
#include <mutex>

namespace nineyin::util {

namespace {
std::mutex g_mu;
LogLevel g_level = LogLevel::Info;

const char* level_name(LogLevel l) {
  switch (l) {
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "?";
}
}  // namespace

void set_log_level(LogLevel level) { g_level = level; }

void log(LogLevel level, const char* fmt, ...) {
  if (level < g_level) return;

  std::lock_guard lock(g_mu);

  std::time_t now = std::time(nullptr);
  char timebuf[32];
  std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

  std::fprintf(stderr, "[%s] [%s] ", timebuf, level_name(level));

  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(stderr, fmt, ap);
  va_end(ap);

  std::fputc('\n', stderr);
  std::fflush(stderr);
}

}  // namespace nineyin::util
