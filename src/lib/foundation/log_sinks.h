#ifndef ATLAS_LIB_FOUNDATION_LOG_SINKS_H_
#define ATLAS_LIB_FOUNDATION_LOG_SINKS_H_

#include <filesystem>
#include <memory>

#include "foundation/log.h"

namespace atlas {

class ConsoleSink : public LogSink {
 public:
  void Write(LogLevel level, std::string_view category, std::string_view message,
             const std::source_location& location) override;
  void Flush() override;
};

class FileSink : public LogSink {
 public:
  explicit FileSink(const std::filesystem::path& path);
  ~FileSink() override;

  FileSink(const FileSink&) = delete;
  FileSink& operator=(const FileSink&) = delete;

  void Write(LogLevel level, std::string_view category, std::string_view message,
             const std::source_location& location) override;
  void Flush() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_LOG_SINKS_H_
