#pragma once

#include "foundation/error.hpp"
#include "foundation/log.hpp"

#include <atomic>
#include <filesystem>

namespace atlas
{

// ============================================================================
// RuntimeConfig — options passed to Runtime::initialize()
// ============================================================================

struct RuntimeConfig
{
    LogLevel log_level = LogLevel::Info;
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
//   auto result = atlas::Runtime::initialize({.log_level = atlas::LogLevel::Debug});
//   if (!result) { /* handle error */ }
//   // ... run server ...
//   atlas::Runtime::finalize();
//
// The existing per-subsystem init functions continue to work as-is. Runtime
// simply calls them in the correct order, guaranteeing a consistent startup
// sequence across all process types.

class Runtime
{
public:
    // Initialise all Atlas subsystems in dependency order.
    // Returns an error if called a second time without an intervening finalize().
    [[nodiscard]] static auto initialize(const RuntimeConfig& config = {}) -> Result<void>;

    // Tear down all subsystems in reverse order.
    // Safe to call even if initialize() was never called.
    static void finalize();

    [[nodiscard]] static auto is_initialized() -> bool
    {
        return initialized_.load(std::memory_order_acquire);
    }

private:
    static std::atomic<bool> initialized_;
};

}  // namespace atlas
