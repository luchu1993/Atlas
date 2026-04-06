#pragma once

#include "foundation/log.hpp"

#include <filesystem>
#include <memory>

namespace atlas
{

class ConsoleSink : public LogSink
{
public:
    void write(LogLevel level, std::string_view category, std::string_view message,
               const std::source_location& location) override;
    void flush() override;
};

class FileSink : public LogSink
{
public:
    explicit FileSink(const std::filesystem::path& path);
    ~FileSink() override;

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;

    void write(LogLevel level, std::string_view category, std::string_view message,
               const std::source_location& location) override;
    void flush() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace atlas
