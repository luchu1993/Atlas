#pragma once

#include "clrscript/file_watcher.hpp"
#include "foundation/error.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>

namespace atlas
{

class ClrScriptEngine;

/// C++ side hot-reload manager. Monitors script files, compiles, and
/// coordinates with C# HotReloadManager for state migration.
class ClrHotReload
{
public:
    struct Config
    {
        std::filesystem::path script_project_path;  // .csproj or directory
        std::filesystem::path output_directory;     // compiled DLL output
        std::chrono::milliseconds debounce_delay{500};
        std::chrono::seconds unload_timeout{5};
        bool auto_compile{true};
        bool enabled{false};
    };

    explicit ClrHotReload(ClrScriptEngine& engine);

    [[nodiscard]] auto configure(const Config& config) -> Result<void>;

    /// Manually trigger a hot-reload.
    [[nodiscard]] auto reload() -> Result<void>;

    /// Check for pending file changes (call from main loop).
    /// Respects debounce delay.
    void poll();

    /// Process a pending reload if one is queued (call from main loop between ticks).
    [[nodiscard]] auto process_pending() -> Result<void>;

    [[nodiscard]] auto is_enabled() const -> bool { return config_.enabled; }

private:
    ClrScriptEngine& engine_;
    Config config_;
    std::optional<FileWatcher> watcher_;
    std::atomic<bool> pending_reload_{false};

    using Clock = std::chrono::steady_clock;
    Clock::time_point last_change_time_{};
    bool debouncing_{false};

    [[nodiscard]] auto compile_scripts() -> Result<void>;
    [[nodiscard]] auto do_reload() -> Result<void>;
};

}  // namespace atlas
