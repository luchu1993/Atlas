#include "foundation/log_sinks.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace atlas
{

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------

void ConsoleSink::write(LogLevel level, std::string_view category,
                        std::string_view message,
                        const std::source_location& location)
{
    const char* filename = location.file_name();

    std::ostringstream oss;
    if (category.empty())
    {
        oss << "[" << log_level_name(level) << "] "
            << "[" << filename << ":" << location.line() << "] "
            << message << "\n";
    }
    else
    {
        oss << "[" << log_level_name(level) << "] "
            << "[" << category << "] "
            << "[" << filename << ":" << location.line() << "] "
            << message << "\n";
    }

    std::string formatted = oss.str();

    if (level >= LogLevel::Error)
    {
        fprintf(stderr, "%s", formatted.c_str());
    }
    else
    {
        fprintf(stdout, "%s", formatted.c_str());
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

FileSink::FileSink(const std::filesystem::path& path)
    : impl_(std::make_unique<Impl>())
{
    impl_->file_.open(path, std::ios::app);
    if (!impl_->file_.is_open())
    {
        fprintf(stderr, "[WARNING] Failed to open log file: %s\n",
                path.string().c_str());
    }
}

FileSink::~FileSink() = default;

void FileSink::write(LogLevel level, std::string_view category,
                     std::string_view message,
                     const std::source_location& location)
{
    if (!impl_->file_.is_open())
    {
        return;
    }

    const char* filename = location.file_name();

    try
    {
        if (category.empty())
        {
            impl_->file_ << "[" << log_level_name(level) << "] "
                          << "[" << filename << ":" << location.line() << "] "
                          << message << "\n";
        }
        else
        {
            impl_->file_ << "[" << log_level_name(level) << "] "
                          << "[" << category << "] "
                          << "[" << filename << ":" << location.line() << "] "
                          << message << "\n";
        }
    }
    catch (...)
    {
        // Silently ignore ofstream failures
    }
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

} // namespace atlas
