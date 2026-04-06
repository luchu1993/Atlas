#pragma once

#include "platform/platform_config.hpp"

#include <cstdint>
#include <functional>

namespace atlas
{

// ============================================================================
// Signal
// ============================================================================

enum class Signal : uint8_t
{
    Interrupt,  // SIGINT / Ctrl+C
    Terminate,  // SIGTERM
    Hangup,     // SIGHUP (Unix) / CTRL_CLOSE_EVENT (Win)
    User1,      // SIGUSR1 (Unix only)
    User2,      // SIGUSR2 (Unix only)
};

using SignalCallback = std::function<void(Signal)>;

void install_signal_handler(Signal sig, SignalCallback callback);
void remove_signal_handler(Signal sig);

}  // namespace atlas
