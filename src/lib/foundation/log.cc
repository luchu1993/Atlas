#include "foundation/log.h"

namespace atlas {

auto LogLevelName(LogLevel level) -> std::string_view {
  switch (level) {
    case LogLevel::kTrace:
      return "TRACE";
    case LogLevel::kDebug:
      return "DEBUG";
    case LogLevel::kInfo:
      return "INFO";
    case LogLevel::kWarning:
      return "WARNING";
    case LogLevel::kError:
      return "ERROR";
    case LogLevel::kCritical:
      return "CRITICAL";
    case LogLevel::kOff:
      return "OFF";
  }
  return "UNKNOWN";
}

auto Logger::Instance() -> Logger& {
  static Logger s_instance;
  return s_instance;
}

Logger::Logger() = default;

void Logger::SetLevel(LogLevel level) {
  runtime_level_.store(level, std::memory_order_release);
}

auto Logger::Level() const -> LogLevel {
  return runtime_level_.load(std::memory_order_acquire);
}

void Logger::AddSink(std::shared_ptr<LogSink> sink) {
  std::lock_guard lock(mutex_);
  // Copy current list, append, publish atomically.
  auto old = sinks_.load(std::memory_order_acquire);
  auto next = std::make_shared<SinkList>(*old);
  next->push_back(std::move(sink));
  sinks_.store(std::move(next), std::memory_order_release);
}

void Logger::ClearSinks() {
  std::lock_guard lock(mutex_);
  sinks_.store(std::make_shared<SinkList>(), std::memory_order_release);
}

void Logger::Log(LogLevel level, std::string_view category, std::string_view message,
                 const std::source_location& location) {
  // Lock-free snapshot load: no mutex, no per-sink atomic refcount churn
  // beyond the single shared_ptr load.
  auto snapshot = sinks_.load(std::memory_order_acquire);
  for (auto& sink : *snapshot) {
    sink->Write(level, category, message, location);
  }
}

void Logger::Flush() {
  auto snapshot = sinks_.load(std::memory_order_acquire);
  for (auto& sink : *snapshot) {
    sink->Flush();
  }
}

}  // namespace atlas
