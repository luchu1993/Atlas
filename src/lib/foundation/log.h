#ifndef ATLAS_LIB_FOUNDATION_LOG_H_
#define ATLAS_LIB_FOUNDATION_LOG_H_

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/platform_config.h"

namespace atlas {

enum class LogLevel : uint8_t {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarning = 3,
  kError = 4,
  kCritical = 5,
  kOff = 6,
};

[[nodiscard]] auto LogLevelName(LogLevel level) -> std::string_view;

#if ATLAS_DEBUG
inline constexpr LogLevel kCompileTimeMinLevel = LogLevel::kTrace;
#else
inline constexpr LogLevel kCompileTimeMinLevel = LogLevel::kInfo;
#endif

class LogSink {
 public:
  virtual ~LogSink() = default;
  virtual void Write(LogLevel level, std::string_view category, std::string_view message,
                     const std::source_location& location) = 0;
  virtual void Flush() = 0;
};

// Thread safety: Thread-safe. Sinks are snapshot-copied before invocation
// to avoid deadlock if a sink calls back into the logger.
class Logger {
 public:
  static auto Instance() -> Logger&;

  void SetLevel(LogLevel level);
  [[nodiscard]] auto Level() const -> LogLevel;

  void AddSink(std::shared_ptr<LogSink> sink);
  void ClearSinks();

  void Log(LogLevel level, std::string_view category, std::string_view message,
           const std::source_location& location = std::source_location::current());

  void Flush();

 private:
  Logger();

  // Sinks are published as an immutable snapshot via atomic shared_ptr.
  // log() does a single lock-free load; writers serialize with mutex_.
  using SinkList = std::vector<std::shared_ptr<LogSink>>;
  mutable std::mutex mutex_;  // guards sink list mutations only
  // runtime_level_ is read on every log call; use atomic to avoid mutex
  // contention on the hot path.  LogLevel is uint8_t so this is lock-free.
  std::atomic<LogLevel> runtime_level_{LogLevel::kTrace};
  std::atomic<std::shared_ptr<SinkList>> sinks_{std::make_shared<SinkList>()};
};

// Proxy object to forward format args to std::format properly (avoids MSVC macro issues)
class LogProxy {
 public:
  LogProxy(LogLevel level, std::string_view category, std::source_location loc)
      : level_(level), category_(category), location_(loc) {}

  template <typename... Args>
  void operator()(std::format_string<Args...> fmt, Args&&... args) const {
    auto& logger = Logger::Instance();
    if (static_cast<uint8_t>(level_) >= static_cast<uint8_t>(logger.Level())) {
      logger.Log(level_, category_, std::format(fmt, std::forward<Args>(args)...), location_);
    }
  }

 private:
  LogLevel level_;
  std::string_view category_;
  std::source_location location_;
};

}  // namespace atlas

// Logging macros with compile-time and runtime level filtering.
// Arguments are NOT evaluated if filtered at compile time.

#define ATLAS_LOG(level, category, ...)                                                 \
  do {                                                                                  \
    if constexpr (static_cast<uint8_t>(level) >=                                        \
                  static_cast<uint8_t>(::atlas::kCompileTimeMinLevel)) {                \
      ::atlas::LogProxy(level, category, std::source_location::current())(__VA_ARGS__); \
    }                                                                                   \
  } while (false)

#define ATLAS_LOG_TRACE(...) ATLAS_LOG(::atlas::LogLevel::kTrace, "", __VA_ARGS__)
#define ATLAS_LOG_DEBUG(...) ATLAS_LOG(::atlas::LogLevel::kDebug, "", __VA_ARGS__)
#define ATLAS_LOG_INFO(...) ATLAS_LOG(::atlas::LogLevel::kInfo, "", __VA_ARGS__)
#define ATLAS_LOG_WARNING(...) ATLAS_LOG(::atlas::LogLevel::kWarning, "", __VA_ARGS__)
#define ATLAS_LOG_ERROR(...) ATLAS_LOG(::atlas::LogLevel::kError, "", __VA_ARGS__)
#define ATLAS_LOG_CRITICAL(...) ATLAS_LOG(::atlas::LogLevel::kCritical, "", __VA_ARGS__)

#endif  // ATLAS_LIB_FOUNDATION_LOG_H_
