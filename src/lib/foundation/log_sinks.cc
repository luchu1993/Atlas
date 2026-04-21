#include "foundation/log_sinks.h"

#include <cstdio>
#include <format>
#include <fstream>

namespace atlas {

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------

void ConsoleSink::Write(LogLevel level, std::string_view category, std::string_view message,
                        const std::source_location& location) {
  std::string formatted;
  if (category.empty()) {
    formatted = std::format("[{}] [{}:{}] {}\n", LogLevelName(level), location.file_name(),
                            location.line(), message);
  } else {
    formatted = std::format("[{}] [{}] [{}:{}] {}\n", LogLevelName(level), category,
                            location.file_name(), location.line(), message);
  }

  // fflush on every line, for both streams. Without this, stdout stays in
  // the 4 KB C-runtime buffer when the process is redirected to a pipe —
  // a harness that TerminateProcess'es the child (Windows world_stress
  // script-client flow) loses every INFO log emitted before exit because
  // TerminateProcess skips the C-runtime's exit-time flush. The cost is
  // one fflush per log line, which is negligible for operator-facing
  // diagnostic volume.
  if (level >= LogLevel::kError) {
    std::fwrite(formatted.data(), 1, formatted.size(), stderr);
    std::fflush(stderr);
  } else {
    std::fwrite(formatted.data(), 1, formatted.size(), stdout);
    std::fflush(stdout);
  }
}

void ConsoleSink::Flush() {
  fflush(stdout);
  fflush(stderr);
}

// ---------------------------------------------------------------------------
// FileSink
// ---------------------------------------------------------------------------

struct FileSink::Impl {
  std::ofstream file;
};

FileSink::FileSink(const std::filesystem::path& path) : impl_(std::make_unique<Impl>()) {
  impl_->file.open(path, std::ios::app);
  if (!impl_->file.is_open()) {
    fprintf(stderr, "[WARNING] Failed to open log file: %s\n", path.string().c_str());
  }
}

FileSink::~FileSink() = default;

void FileSink::Write(LogLevel level, std::string_view category, std::string_view message,
                     const std::source_location& location) {
  if (!impl_->file.is_open()) {
    return;
  }

  std::string formatted;
  if (category.empty()) {
    formatted = std::format("[{}] [{}:{}] {}\n", LogLevelName(level), location.file_name(),
                            location.line(), message);
  } else {
    formatted = std::format("[{}] [{}] [{}:{}] {}\n", LogLevelName(level), category,
                            location.file_name(), location.line(), message);
  }

  impl_->file.write(formatted.data(), static_cast<std::streamsize>(formatted.size()));
}

void FileSink::Flush() {
  if (!impl_->file.is_open()) {
    return;
  }

  try {
    impl_->file.flush();
  } catch (...) {
    // Silently ignore ofstream failures
  }
}

}  // namespace atlas
