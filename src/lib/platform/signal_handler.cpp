#include "platform/signal_handler.hpp"

#include <array>
#include <csignal>
#include <mutex>

#if ATLAS_PLATFORM_WINDOWS
#include <windows.h>
#endif

namespace atlas
{

namespace
{

// ============================================================================
// Global state
// ============================================================================

constexpr std::size_t k_signal_count = 5;

// Callbacks — only accessed from non-signal contexts (under g_mutex).
std::array<SignalCallback, k_signal_count> g_handlers{};
std::mutex g_mutex;

// Pending flags — written by OS signal handlers (async-signal-safe: only
// plain reads/writes to volatile sig_atomic_t are permitted inside a signal
// handler).  Read and cleared by dispatch_pending_signals() in the main loop.
volatile sig_atomic_t g_pending[k_signal_count]{};

// ============================================================================
// Helpers
// ============================================================================

[[nodiscard]] auto signal_index(Signal sig) -> std::size_t
{
    auto idx = static_cast<std::size_t>(sig);
    if (idx >= k_signal_count)
    {
        idx = 0;
    }
    return idx;
}

// Called only from async-signal-safe context: just set the flag.
void mark_pending(Signal sig)
{
    g_pending[signal_index(sig)] = 1;
}

// ============================================================================
// Platform: Windows
// ============================================================================

#if ATLAS_PLATFORM_WINDOWS

void c_signal_handler(int signum)
{
    switch (signum)
    {
        case SIGINT:
            mark_pending(Signal::Interrupt);
            break;
        case SIGTERM:
            mark_pending(Signal::Terminate);
            break;
        default:
            break;
    }
}

BOOL WINAPI console_ctrl_handler(DWORD ctrl_type)
{
    switch (ctrl_type)
    {
        case CTRL_C_EVENT:
            mark_pending(Signal::Interrupt);
            return TRUE;
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            mark_pending(Signal::Hangup);
            return TRUE;
        case CTRL_BREAK_EVENT:
            mark_pending(Signal::Terminate);
            return TRUE;
        default:
            return FALSE;
    }
}

static bool g_console_handler_installed = false;

void platform_install(Signal sig)
{
    switch (sig)
    {
        case Signal::Interrupt:
            std::signal(SIGINT, c_signal_handler);
            if (!g_console_handler_installed)
            {
                SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
                g_console_handler_installed = true;
            }
            break;
        case Signal::Terminate:
            std::signal(SIGTERM, c_signal_handler);
            break;
        case Signal::Hangup:
            if (!g_console_handler_installed)
            {
                SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
                g_console_handler_installed = true;
            }
            break;
        case Signal::User1:
        case Signal::User2:
            // Not supported on Windows
            break;
    }
}

void platform_remove(Signal sig)
{
    switch (sig)
    {
        case Signal::Interrupt:
            std::signal(SIGINT, SIG_DFL);
            break;
        case Signal::Terminate:
            std::signal(SIGTERM, SIG_DFL);
            break;
        case Signal::Hangup:
            // Console handler is shared; only remove if no Interrupt handler either
            break;
        case Signal::User1:
        case Signal::User2:
            break;
    }
}

// ============================================================================
// Platform: Linux / POSIX
// ============================================================================

#elif ATLAS_PLATFORM_LINUX

[[nodiscard]] auto signal_to_signum(Signal sig) -> int
{
    switch (sig)
    {
        case Signal::Interrupt:
            return SIGINT;
        case Signal::Terminate:
            return SIGTERM;
        case Signal::Hangup:
            return SIGHUP;
        case Signal::User1:
            return SIGUSR1;
        case Signal::User2:
            return SIGUSR2;
    }
    return -1;
}

[[nodiscard]] auto signum_to_signal(int signum) -> Signal
{
    switch (signum)
    {
        case SIGINT:
            return Signal::Interrupt;
        case SIGTERM:
            return Signal::Terminate;
        case SIGHUP:
            return Signal::Hangup;
        case SIGUSR1:
            return Signal::User1;
        case SIGUSR2:
            return Signal::User2;
        default:
            return Signal::Interrupt;  // fallback
    }
}

// Async-signal-safe: only writes a volatile sig_atomic_t flag.
void posix_signal_handler(int signum)
{
    mark_pending(signum_to_signal(signum));
}

void platform_install(Signal sig)
{
    int signum = signal_to_signum(sig);
    if (signum < 0)
    {
        return;
    }

    struct sigaction sa{};
    sa.sa_handler = posix_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(signum, &sa, nullptr);
}

void platform_remove(Signal sig)
{
    int signum = signal_to_signum(sig);
    if (signum < 0)
    {
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

// ============================================================================
// Public API
// ============================================================================

void install_signal_handler(Signal sig, SignalCallback callback)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handlers[signal_index(sig)] = std::move(callback);
    platform_install(sig);
}

void remove_signal_handler(Signal sig)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_handlers[signal_index(sig)] = nullptr;
    platform_remove(sig);
}

void dispatch_pending_signals()
{
    for (std::size_t i = 0; i < k_signal_count; ++i)
    {
        if (g_pending[i])
        {
            g_pending[i] = 0;

            SignalCallback cb;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                cb = g_handlers[i];
            }
            if (cb)
            {
                cb(static_cast<Signal>(i));
            }
        }
    }
}

}  // namespace atlas
