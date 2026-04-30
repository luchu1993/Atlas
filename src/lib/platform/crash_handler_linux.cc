#include "platform/crash_handler.h"

#if ATLAS_PLATFORM_LINUX

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace atlas {

namespace crash_internal {

extern std::mutex g_install_mutex;
extern CrashHandlerOptions g_opts;
extern bool g_installed;
extern char g_dump_dir_buf[512];
extern std::size_t g_dump_dir_len;
extern char g_process_name_buf[64];
bool PrepareDumpDir(const std::string& dir);
void FormatDumpStem(char* out, std::size_t out_size, int pid);

constexpr std::size_t kAltStackSize = 128 * 1024;
char g_alt_stack[kAltStackSize];

constexpr int kFatalSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
struct sigaction g_prev_actions[sizeof(kFatalSignals) / sizeof(kFatalSignals[0])]{};

// Async-signal-safe write of a NUL-terminated string.
void SafeWrite(int fd, const char* s) {
  std::size_t n = 0;
  while (s[n] != '\0') ++n;

  const char* p = s;
  while (n > 0) {
    const ssize_t written = ::write(fd, p, n);
    if (written <= 0) return;
    p += written;
    n -= static_cast<std::size_t>(written);
  }
}

void BuildDumpPath(char* out, std::size_t out_size) {
  char stem[128];
  FormatDumpStem(stem, sizeof(stem), static_cast<int>(::getpid()));
  std::snprintf(out, out_size, "%s%s.crash", g_dump_dir_buf, stem);
}

const char* SignalName(int sig) {
  switch (sig) {
    case SIGSEGV:
      return "SIGSEGV";
    case SIGABRT:
      return "SIGABRT";
    case SIGBUS:
      return "SIGBUS";
    case SIGFPE:
      return "SIGFPE";
    case SIGILL:
      return "SIGILL";
    default:
      return "UNKNOWN";
  }
}

void WriteCrashFile(int fd, int sig, siginfo_t* info, void* /*ucontext*/) {
  SafeWrite(fd, "Atlas crash report\n");
  SafeWrite(fd, "process: ");
  SafeWrite(fd, g_process_name_buf);
  SafeWrite(fd, "\nsignal: ");
  SafeWrite(fd, SignalName(sig));
  SafeWrite(fd, "\n");

  if (info) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "si_code: %d\nfault_addr: %p\n", info->si_code, info->si_addr);
    SafeWrite(fd, buf);
  }

  // backtrace() is not strictly async-signal-safe, but in practice it works
  // for SEGV/ABRT and is the simplest option without pulling in breakpad.
  void* frames[64];
  int n = ::backtrace(frames, 64);
  SafeWrite(fd, "backtrace:\n");
  ::backtrace_symbols_fd(frames, n, fd);
}

void Handler(int sig, siginfo_t* info, void* ucontext) {
  char dump_path[1024];
  BuildDumpPath(dump_path, sizeof(dump_path));

  int fd = ::open(dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    WriteCrashFile(fd, sig, info, ucontext);
    ::close(fd);

    if (g_opts.on_crash) {
      // Best-effort.  Caller is responsible for not making the situation
      // worse from inside an async-signal handler.
      g_opts.on_crash(std::string(dump_path));
    }
  }

  // Restore default and re-raise so core dumps / exit codes behave normally.
  for (std::size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); ++i) {
    if (kFatalSignals[i] == sig) {
      ::sigaction(sig, &g_prev_actions[i], nullptr);
      break;
    }
  }
  ::raise(sig);
}

}  // namespace crash_internal

// ============================================================================
// Public API
// ============================================================================

bool InstallCrashHandler(const CrashHandlerOptions& opts) {
  using namespace crash_internal;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  if (g_installed) return true;

  g_opts = opts;

  if (!PrepareDumpDir(opts.dump_dir)) return false;

  std::snprintf(g_process_name_buf, sizeof(g_process_name_buf), "%s",
                opts.process_name.empty() ? "process" : opts.process_name.c_str());

  // Alternate signal stack so SIGSEGV from stack overflow can still run.
  stack_t ss{};
  ss.ss_sp = g_alt_stack;
  ss.ss_size = kAltStackSize;
  ss.ss_flags = 0;
  ::sigaltstack(&ss, nullptr);

  struct sigaction sa{};
  sa.sa_sigaction = Handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;

  for (std::size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); ++i) {
    ::sigaction(kFatalSignals[i], &sa, &g_prev_actions[i]);
  }

  g_installed = true;
  return true;
}

void UninstallCrashHandler() {
  using namespace crash_internal;
  std::lock_guard<std::mutex> lock(g_install_mutex);
  if (!g_installed) return;

  for (std::size_t i = 0; i < sizeof(kFatalSignals) / sizeof(kFatalSignals[0]); ++i) {
    ::sigaction(kFatalSignals[i], &g_prev_actions[i], nullptr);
  }
  g_installed = false;
}

std::string WriteCrashDumpForTesting() {
  using namespace crash_internal;
  if (!g_installed) return {};

  char dump_path[1024];
  BuildDumpPath(dump_path, sizeof(dump_path));

  int fd = ::open(dump_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return {};

  WriteCrashFile(fd, 0, nullptr, nullptr);
  ::close(fd);
  return std::string(dump_path);
}

}  // namespace atlas

#endif  // ATLAS_PLATFORM_LINUX
