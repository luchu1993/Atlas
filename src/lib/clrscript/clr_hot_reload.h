#ifndef ATLAS_LIB_CLRSCRIPT_CLR_HOT_RELOAD_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_HOT_RELOAD_H_

#include <atomic>
#include <chrono>
#include <filesystem>
#include <optional>

#include "clrscript/file_watcher.h"
#include "foundation/error.h"

namespace atlas {

class ClrScriptEngine;

class ClrHotReload {
 public:
  struct Config {
    std::filesystem::path script_project_path;  // .csproj or directory
    std::filesystem::path output_directory;     // compiled DLL output
    std::string assembly_name{"Atlas.GameScripts.dll"};
    std::chrono::milliseconds debounce_delay{500};
    std::chrono::seconds unload_timeout{5};
    bool auto_compile{true};
    bool enabled{false};
  };

  explicit ClrHotReload(ClrScriptEngine& engine);

  [[nodiscard]] auto Configure(const Config& config) -> Result<void>;

  [[nodiscard]] auto Reload() -> Result<void>;

  void Poll();

  [[nodiscard]] auto ProcessPending() -> Result<void>;

  [[nodiscard]] auto IsEnabled() const -> bool { return config_.enabled; }

 private:
  ClrScriptEngine& engine_;
  Config config_;
  std::optional<FileWatcher> watcher_;
  std::atomic<bool> pending_reload_{false};

  using Clock = std::chrono::steady_clock;
  Clock::time_point last_change_time_{};
  bool debouncing_{false};

  [[nodiscard]] auto CompileScripts() const -> Result<void>;
  [[nodiscard]] auto DoReload() -> Result<void>;
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_HOT_RELOAD_H_
