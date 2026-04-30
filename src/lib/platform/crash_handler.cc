// MSVC marks std::getenv as deprecated in favour of _dupenv_s, but the
// crash handler runs at startup with no concurrent env mutation, so the
// portable spelling is fine.
#define _CRT_SECURE_NO_WARNINGS

#include "platform/crash_handler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>

namespace atlas {
namespace crash_internal {

// A mutex guards installation; the crash path itself does
// NOT take the mutex (locking inside a signal/SEH handler is unsafe).
std::mutex g_install_mutex;

CrashHandlerOptions g_opts;
bool g_installed = false;

// Pre-resolved at install time so the crash path can avoid filesystem /
// std::format work that may allocate.  dump_dir_buf includes a trailing
// path separator and a NUL terminator.
char g_dump_dir_buf[512] = {};
std::size_t g_dump_dir_len = 0;
char g_process_name_buf[64] = {};

bool PrepareDumpDir(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return false;
  }

  // Cache an absolute path with a trailing separator so the crash path can
  // build "<dir>/<file>" with a single memcpy + snprintf.
  auto abs = std::filesystem::absolute(dir, ec).string();
  if (ec) {
    abs = dir;
  }
  if (!abs.empty() && abs.back() != '/' && abs.back() != '\\') {
    abs.push_back('/');
  }
  if (abs.size() + 1 >= sizeof(g_dump_dir_buf)) {
    return false;
  }
  std::memcpy(g_dump_dir_buf, abs.data(), abs.size());
  g_dump_dir_buf[abs.size()] = '\0';
  g_dump_dir_len = abs.size();
  return true;
}

// Best-effort crash-path helper: localtime + snprintf only.
void FormatDumpStem(char* out, std::size_t out_size, int pid) {
  std::time_t now = std::time(nullptr);
  std::tm tm_buf{};
#if defined(_WIN32)
  localtime_s(&tm_buf, &now);
#else
  localtime_r(&now, &tm_buf);
#endif
  std::snprintf(out, out_size, "%s_%d_%04d%02d%02d-%02d%02d%02d", g_process_name_buf, pid,
                tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday, tm_buf.tm_hour,
                tm_buf.tm_min, tm_buf.tm_sec);
}

}  // namespace crash_internal

bool InstallDefaultCrashHandler(const std::string& process_name) {
  CrashHandlerOptions opts;
  opts.process_name = process_name;

  if (const char* env = std::getenv("ATLAS_DUMP_DIR"); env && env[0] != '\0') {
    opts.dump_dir = env;
  } else {
    opts.dump_dir = ".tmp/dumps";
  }

  if (const char* env = std::getenv("ATLAS_DUMP_FULL"); env && env[0] == '1') {
    opts.full_memory = true;
  }

  std::string tag = process_name;
  opts.on_crash = [tag](const std::string& path) {
    std::fprintf(stderr, "[%s] crash dump written: %s\n", tag.c_str(), path.c_str());
    std::fflush(stderr);
  };

  return InstallCrashHandler(opts);
}

}  // namespace atlas
