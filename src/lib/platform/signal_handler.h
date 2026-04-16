#ifndef ATLAS_LIB_PLATFORM_SIGNAL_HANDLER_H_
#define ATLAS_LIB_PLATFORM_SIGNAL_HANDLER_H_

#include <cstdint>
#include <functional>

#include "platform/platform_config.h"

namespace atlas {

// ============================================================================
// Signal
// ============================================================================

enum class Signal : uint8_t {
  kInterrupt,  // SIGINT / Ctrl+C
  kTerminate,  // SIGTERM
  kHangup,     // SIGHUP (Unix) / CTRL_CLOSE_EVENT (Win)
  kUser1,      // SIGUSR1 (Unix only)
  kUser2,      // SIGUSR2 (Unix only)
};

using SignalCallback = std::function<void(Signal)>;

void InstallSignalHandler(Signal sig, SignalCallback callback);
void RemoveSignalHandler(Signal sig);

// Must be called periodically from the main event loop (e.g. EventDispatcher).
// The OS signal handlers only set a pending flag; this function dispatches the
// registered callbacks in a safe (non-signal) context.
void DispatchPendingSignals();

}  // namespace atlas

#endif  // ATLAS_LIB_PLATFORM_SIGNAL_HANDLER_H_
