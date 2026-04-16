#ifndef ATLAS_LIB_FOUNDATION_RUNTIME_H_
#define ATLAS_LIB_FOUNDATION_RUNTIME_H_

#include <atomic>
#include <filesystem>
#include <mutex>

#include "foundation/error.h"
#include "foundation/log.h"

namespace atlas {

// ============================================================================
// RuntimeConfig — options passed to Runtime::initialize()
// ============================================================================

struct RuntimeConfig {
  LogLevel log_level = LogLevel::kInfo;
  bool enable_file_logging = false;
  std::filesystem::path log_file;
};

// ============================================================================
// Runtime — lightweight coordinator for Atlas global state
// ============================================================================
//
// Provides a single, ordered entry point for initialising all subsystems that
// require explicit setup (logging, signal handling, platform sockets, etc.).
//
// Usage (in each server process main()):
//
//   auto result = atlas::Runtime::Initialize({.log_level = atlas::LogLevel::kDebug});
//   if (!result) { /* handle error */ }
//   // ... run server ...
//   atlas::Runtime::finalize();
//
// The existing per-subsystem init functions continue to work as-is. Runtime
// simply calls them in the correct order, guaranteeing a consistent startup
// sequence across all process types.
//
// initialize()/finalize() are reference-counted so multi-component integration
// tests can host multiple logical server processes inside one OS process
// without fighting over the global logger/runtime state.

class Runtime {
 public:
  // Initialise all Atlas subsystems in dependency order.
  // Returns an error if called a second time without an intervening finalize().
  [[nodiscard]] static auto Initialize(const RuntimeConfig& config = {}) -> Result<void>;

  // Tear down all subsystems in reverse order.
  // Safe to call even if initialize() was never called.
  static void Finalize();

  [[nodiscard]] static auto IsInitialized() -> bool {
    return init_count.load(std::memory_order_acquire) > 0;
  }

 private:
  static std::atomic<uint32_t> init_count;
  static std::mutex mutex;
};

}  // namespace atlas

#endif  // ATLAS_LIB_FOUNDATION_RUNTIME_H_
