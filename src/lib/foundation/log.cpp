#include "foundation/log.hpp"

namespace atlas
{

auto log_level_name(LogLevel level) -> std::string_view
{
    switch (level)
    {
        case LogLevel::Trace:
            return "TRACE";
        case LogLevel::Debug:
            return "DEBUG";
        case LogLevel::Info:
            return "INFO";
        case LogLevel::Warning:
            return "WARNING";
        case LogLevel::Error:
            return "ERROR";
        case LogLevel::Critical:
            return "CRITICAL";
        case LogLevel::Off:
            return "OFF";
    }
    return "UNKNOWN";
}

auto Logger::instance() -> Logger&
{
    static Logger s_instance;
    return s_instance;
}

Logger::Logger() = default;

void Logger::set_level(LogLevel level)
{
    std::lock_guard lock(mutex_);
    runtime_level_ = level;
}

auto Logger::level() const -> LogLevel
{
    std::lock_guard lock(mutex_);
    return runtime_level_;
}

void Logger::add_sink(std::shared_ptr<LogSink> sink)
{
    std::lock_guard lock(mutex_);
    // Copy current list, append, publish atomically.
    auto old = sinks_.load(std::memory_order_acquire);
    auto next = std::make_shared<SinkList>(*old);
    next->push_back(std::move(sink));
    sinks_.store(std::move(next), std::memory_order_release);
}

void Logger::clear_sinks()
{
    std::lock_guard lock(mutex_);
    sinks_.store(std::make_shared<SinkList>(), std::memory_order_release);
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message,
                 const std::source_location& location)
{
    // Lock-free snapshot load: no mutex, no per-sink atomic refcount churn
    // beyond the single shared_ptr load.
    auto snapshot = sinks_.load(std::memory_order_acquire);
    for (auto& sink : *snapshot)
    {
        sink->write(level, category, message, location);
    }
}

void Logger::flush()
{
    auto snapshot = sinks_.load(std::memory_order_acquire);
    for (auto& sink : *snapshot)
    {
        sink->flush();
    }
}

}  // namespace atlas
