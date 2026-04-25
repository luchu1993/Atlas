#include "foundation/log_sinks.h"

#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>

#if ATLAS_PLATFORM_WINDOWS
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace atlas {

namespace {

#if ATLAS_PLATFORM_WINDOWS
// Enable ANSI escape processing on the Windows console.  Called once per
// handle; harmless if the handle is a pipe or file (SetConsoleMode simply
// fails and we fall through to non-colour output).
void EnableAnsiEscapes(FILE* stream) {
  HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD mode = 0;
  if (!GetConsoleMode(h, &mode)) return;  // not a console
  SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
}

void EnsureAnsiSupport() {
  static bool once = [] {
    EnableAnsiEscapes(stdout);
    EnableAnsiEscapes(stderr);
    return true;
  }();
  (void)once;
}
#else
void EnsureAnsiSupport() {}
#endif

// Return just the filename component of a path (e.g. "cellapp.cc").
// Pure pointer arithmetic — no allocation, no copy.
auto Basename(const char* path) -> const char* {
  const char* slash = std::strrchr(path, '/');
  const char* bslash = std::strrchr(path, '\\');
  const char* last = (slash > bslash) ? slash : bslash;
  return last ? last + 1 : path;
}

// ANSI colour escapes per log level, matching spdlog's palette.
// Trace=white, Debug=cyan, Info=green, Warning=yellow, Error=red,
// Critical=bold-red-on-white.
struct ColorPair {
  const char* begin;
  const char* end;
};

auto LevelColor(LogLevel level) -> ColorPair {
  switch (level) {
    case LogLevel::kTrace:
      return {"\033[37m", "\033[0m"};  // white
    case LogLevel::kDebug:
      return {"\033[36m", "\033[0m"};  // cyan
    case LogLevel::kInfo:
      return {"\033[32m", "\033[0m"};  // green
    case LogLevel::kWarning:
      return {"\033[33m", "\033[0m"};  // yellow
    case LogLevel::kError:
      return {"\033[31m", "\033[0m"};  // red
    case LogLevel::kCritical:
      return {"\033[1;31m", "\033[0m"};  // bold red
    default:
      return {"", ""};
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// ConsoleSink
// ---------------------------------------------------------------------------

void ConsoleSink::Write(LogLevel level, std::string_view category, std::string_view message,
                        const std::source_location& location) {
  EnsureAnsiSupport();
  const char* file = Basename(location.file_name());
  auto [color_on, color_off] = LevelColor(level);
  std::string formatted;
  if (category.empty()) {
    formatted = std::format("{}[{}]{} [{}:{}] {}\n", color_on, LogLevelName(level), color_off, file,
                            location.line(), message);
  } else {
    formatted = std::format("{}[{}]{} [{}] [{}:{}] {}\n", color_on, LogLevelName(level), color_off,
                            category, file, location.line(), message);
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

  const char* file = Basename(location.file_name());
  std::string formatted;
  if (category.empty()) {
    formatted =
        std::format("[{}] [{}:{}] {}\n", LogLevelName(level), file, location.line(), message);
  } else {
    formatted = std::format("[{}] [{}] [{}:{}] {}\n", LogLevelName(level), category, file,
                            location.line(), message);
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
