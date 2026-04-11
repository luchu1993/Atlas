#include "foundation/log_sinks.hpp"

#include <cstdio>
#include <format>
#include <fstream>

namespace atlas
{

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------

void ConsoleSink::write(LogLevel level, std::string_view category, std::string_view message,
                        const std::source_location& location)
{
    std::string formatted;
    if (category.empty())
    {
        formatted = std::format("[{}] [{}:{}] {}\n", log_level_name(level), location.file_name(),
                                location.line(), message);
    }
    else
    {
        formatted = std::format("[{}] [{}] [{}:{}] {}\n", log_level_name(level), category,
                                location.file_name(), location.line(), message);
    }

    if (level >= LogLevel::Error)
    {
        std::fwrite(formatted.data(), 1, formatted.size(), stderr);
        std::fflush(stderr);
    }
    else
    {
        std::fwrite(formatted.data(), 1, formatted.size(), stdout);
        std::fflush(stdout);
    }
}

void ConsoleSink::flush()
{
    fflush(stdout);
    fflush(stderr);
}

// ---------------------------------------------------------------------------
// FileSink
// ---------------------------------------------------------------------------

struct FileSink::Impl
{
    std::ofstream file_;
};

FileSink::FileSink(const std::filesystem::path& path) : impl_(std::make_unique<Impl>())
{
    impl_->file_.open(path, std::ios::app);
    if (!impl_->file_.is_open())
    {
        fprintf(stderr, "[WARNING] Failed to open log file: %s\n", path.string().c_str());
    }
}

FileSink::~FileSink() = default;

void FileSink::write(LogLevel level, std::string_view category, std::string_view message,
                     const std::source_location& location)
{
    if (!impl_->file_.is_open())
    {
        return;
    }

    std::string formatted;
    if (category.empty())
    {
        formatted = std::format("[{}] [{}:{}] {}\n", log_level_name(level), location.file_name(),
                                location.line(), message);
    }
    else
    {
        formatted = std::format("[{}] [{}] [{}:{}] {}\n", log_level_name(level), category,
                                location.file_name(), location.line(), message);
    }

    impl_->file_.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
}

void FileSink::flush()
{
    if (!impl_->file_.is_open())
    {
        return;
    }

    try
    {
        impl_->file_.flush();
    }
    catch (...)
    {
        // Silently ignore ofstream failures
    }
}

}  // namespace atlas
