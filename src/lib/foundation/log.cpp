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
    sinks_.push_back(std::move(sink));
}

void Logger::clear_sinks()
{
    std::lock_guard lock(mutex_);
    sinks_.clear();
}

void Logger::log(LogLevel level, std::string_view category, std::string_view message,
                 const std::source_location& location)
{
    std::vector<std::shared_ptr<LogSink>> sinks_copy;
    {
        std::lock_guard lock(mutex_);
        sinks_copy = sinks_;
    }
    for (auto& sink : sinks_copy)
    {
        sink->write(level, category, message, location);
    }
}

void Logger::flush()
{
    std::lock_guard lock(mutex_);
    for (auto& sink : sinks_)
    {
        sink->flush();
    }
}

}  // namespace atlas
