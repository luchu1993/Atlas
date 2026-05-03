#include "platform/signal_handler.h"

#include <array>
#include <csignal>
#include <mutex>

#if ATLAS_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace atlas {

namespace {

constexpr std::size_t kSignalCount = 5;

// Callbacks are only accessed from non-signal contexts under g_mutex.
std::array<SignalCallback, kSignalCount> g_handlers{};
std::mutex g_mutex;

// Signal handlers may only touch volatile sig_atomic_t state.
volatile sig_atomic_t g_pending[kSignalCount]{};

[[nodiscard]] auto SignalIndex(Signal sig) -> std::size_t {
  auto idx = static_cast<std::size_t>(sig);
  if (idx >= kSignalCount) {
    idx = 0;
  }
  return idx;
}

// Called only from async-signal-safe context: just set the flag.
void MarkPending(Signal sig) {
  g_pending[SignalIndex(sig)] = 1;
}

#if ATLAS_PLATFORM_WINDOWS

void CSignalHandler(int signum) {
  switch (signum) {
    case SIGINT:
      MarkPending(Signal::kInterrupt);
      break;
    case SIGTERM:
      MarkPending(Signal::kTerminate);
      break;
    default:
      break;
  }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD ctrl_type) {
  switch (ctrl_type) {
    case CTRL_C_EVENT:
      MarkPending(Signal::kInterrupt);
      return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
      MarkPending(Signal::kHangup);
      return TRUE;
    case CTRL_BREAK_EVENT:
      MarkPending(Signal::kTerminate);
      return TRUE;
    default:
      return FALSE;
  }
}

static bool g_console_handler_installed = false;

void PlatformInstall(Signal sig) {
  switch (sig) {
    case Signal::kInterrupt:
      std::signal(SIGINT, CSignalHandler);
      if (!g_console_handler_installed) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        g_console_handler_installed = true;
      }
      break;
    case Signal::kTerminate:
      std::signal(SIGTERM, CSignalHandler);
      break;
    case Signal::kHangup:
      if (!g_console_handler_installed) {
        SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
        g_console_handler_installed = true;
      }
      break;
    case Signal::kUser1:
    case Signal::kUser2:
      break;
  }
}

void PlatformRemove(Signal sig) {
  switch (sig) {
    case Signal::kInterrupt:
      std::signal(SIGINT, SIG_DFL);
      break;
    case Signal::kTerminate:
      std::signal(SIGTERM, SIG_DFL);
      break;
    case Signal::kHangup:
      break;
    case Signal::kUser1:
    case Signal::kUser2:
      break;
  }
}

#else  // POSIX (Linux, macOS, iOS, Android)

[[nodiscard]] auto SignalToSignum(Signal sig) -> int {
  switch (sig) {
    case Signal::kInterrupt:
      return SIGINT;
    case Signal::kTerminate:
      return SIGTERM;
    case Signal::kHangup:
      return SIGHUP;
    case Signal::kUser1:
      return SIGUSR1;
    case Signal::kUser2:
      return SIGUSR2;
  }
  return -1;
}

[[nodiscard]] auto SignumToSignal(int signum) -> Signal {
  switch (signum) {
    case SIGINT:
      return Signal::kInterrupt;
    case SIGTERM:
      return Signal::kTerminate;
    case SIGHUP:
      return Signal::kHangup;
    case SIGUSR1:
      return Signal::kUser1;
    case SIGUSR2:
      return Signal::kUser2;
    default:
      return Signal::kInterrupt;  // fallback
  }
}

// Async-signal-safe: only writes a volatile sig_atomic_t flag.
void PosixSignalHandler(int signum) {
  MarkPending(SignumToSignal(signum));
}

void PlatformInstall(Signal sig) {
  int signum = SignalToSignum(sig);
  if (signum < 0) {
    return;
  }

  struct sigaction sa{};
  sa.sa_handler = PosixSignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(signum, &sa, nullptr);
}

void PlatformRemove(Signal sig) {
  int signum = SignalToSignum(sig);
  if (signum < 0) {
    return;
  }

  struct sigaction sa{};
  sa.sa_handler = SIG_DFL;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(signum, &sa, nullptr);
}

#endif

}  // anonymous namespace

void InstallSignalHandler(Signal sig, SignalCallback callback) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handlers[SignalIndex(sig)] = std::move(callback);
  PlatformInstall(sig);
}

void RemoveSignalHandler(Signal sig) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handlers[SignalIndex(sig)] = nullptr;
  PlatformRemove(sig);
}

void DispatchPendingSignals() {
  for (std::size_t i = 0; i < kSignalCount; ++i) {
    if (g_pending[i]) {
      g_pending[i] = 0;

      SignalCallback cb;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        cb = g_handlers[i];
      }
      if (cb) {
        cb(static_cast<Signal>(i));
      }
    }
  }
}

}  // namespace atlas
