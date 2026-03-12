#pragma once

#include <cstdio>
#include <format>
#include <mutex>
#include <string>

namespace log {

namespace detail {
inline std::mutex &log_mutex() {
  static std::mutex m;
  return m;
}
} // namespace detail

template <typename... Args>
void println(std::format_string<Args...> fmt, Args &&...args) {
  auto msg = std::format(fmt, std::forward<Args>(args)...);
  std::lock_guard<std::mutex> lock(detail::log_mutex());
  std::puts(msg.c_str());
}

template <typename... Args>
void print(std::format_string<Args...> fmt, Args &&...args) {
  auto msg = std::format(fmt, std::forward<Args>(args)...);
  std::lock_guard<std::mutex> lock(detail::log_mutex());
  std::fputs(msg.c_str(), stdout);
}

inline void println(const char *msg) {
  std::lock_guard<std::mutex> lock(detail::log_mutex());
  std::puts(msg);
}

inline void print(const char *msg) {
  std::lock_guard<std::mutex> lock(detail::log_mutex());
  std::fputs(msg, stdout);
}

} // namespace log
