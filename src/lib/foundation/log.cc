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
  std::unique_lock lock(mutex_);
  // Copy current list, append, publish.
  auto next = std::make_shared<SinkList>(*sinks_);
  next->push_back(std::move(sink));
  sinks_ = std::move(next);
}

void Logger::ClearSinks() {
  std::unique_lock lock(mutex_);
  sinks_ = std::make_shared<SinkList>();
}

void Logger::Log(LogLevel level, std::string_view category, std::string_view message,
                 const std::source_location& location) {
  // Brief shared lock to copy the snapshot; iteration runs unlocked.
  std::shared_ptr<SinkList> snapshot;
  {
    std::shared_lock lock(mutex_);
    snapshot = sinks_;
  }
  for (auto& sink : *snapshot) {
    sink->Write(level, category, message, location);
  }
}

void Logger::Flush() {
  std::shared_ptr<SinkList> snapshot;
  {
    std::shared_lock lock(mutex_);
    snapshot = sinks_;
  }
  for (auto& sink : *snapshot) {
    sink->Flush();
  }
}

}  // namespace atlas
