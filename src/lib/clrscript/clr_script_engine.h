#ifndef ATLAS_LIB_CLRSCRIPT_CLR_SCRIPT_ENGINE_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_SCRIPT_ENGINE_H_

#include <filesystem>
#include <optional>
#include <string_view>

#include "clrscript/clr_bootstrap.h"
#include "clrscript/clr_host.h"
#include "clrscript/clr_invoke.h"
#include "foundation/error.h"
#include "script/script_engine.h"

namespace atlas {

// ============================================================================
// ClrScriptEngine — ScriptEngine implementation backed by .NET CoreCLR
// ============================================================================
//
// Lifecycle:
//   1. configure()   — set paths for runtimeconfig.json and Atlas.Runtime.dll
//   2. initialize()  — start CLR, bootstrap, bind lifecycle methods
//   3. load_module() — load user game-script assemblies
//   4. on_init()     — trigger C# entity initialization
//   5. on_tick()     — per-frame update (called from main loop)
//   6. on_shutdown() — trigger C# entity cleanup
//   7. finalize()    — shut down CLR
//
// All lifecycle methods use the "return 0 = ok, return -1 = error" convention.
// Errors from C# are read via the TLS error bridge (clr_error.hpp).

class ClrScriptEngine final : public ScriptEngine {
 public:
  struct Config {
    std::filesystem::path runtime_config_path;    // runtimeconfig.json
    std::filesystem::path runtime_assembly_path;  // Atlas.Runtime.dll
    std::optional<ClrBootstrapArgs> bootstrap_args;
  };

  [[nodiscard]] auto Configure(const Config& config) -> Result<void>;

  // -- ScriptEngine interface -----------------------------------------------

  [[nodiscard]] auto Initialize() -> Result<void> override;
  void Finalize() override;
  [[nodiscard]] auto LoadModule(const std::filesystem::path& path) -> Result<void> override;
  void OnTick(float dt) override;
  void OnInit(bool is_reload) override;
  void OnShutdown() override;
  [[nodiscard]] auto CallFunction(std::string_view module_name, std::string_view function_name,
                                  std::span<const ScriptValue> args)
      -> Result<ScriptValue> override;
  [[nodiscard]] auto RuntimeName() const -> std::string_view override { return "CLR (.NET 9)"; }

  [[nodiscard]] auto IsInitialized() const -> bool { return initialized_; }

  // -- Hot-reload support (used by ClrHotReload) ----------------------------

  /// Call a no-argument [UnmanagedCallersOnly] method in HotReloadManager.
  [[nodiscard]] auto CallHotReload(std::string_view method_name) -> Result<void>;

  /// Call a path-argument [UnmanagedCallersOnly] method in HotReloadManager.
  [[nodiscard]] auto CallHotReload(std::string_view method_name,
                                   const std::filesystem::path& assembly_path) -> Result<void>;

  /// Access the CLR host (needed for rebinding methods after reload).
  [[nodiscard]] auto Host() -> ClrHost& { return host_; }

 private:
  Config config_;
  ClrHost host_;
  bool configured_{false};
  bool initialized_{false};

  // Cached C# [UnmanagedCallersOnly] entry points in Atlas.Runtime.dll.
  // Bound during initialize() after CLR bootstrap.
  ClrFallibleMethod<> engine_init_;      // Lifecycle.EngineInit()
  ClrFallibleMethod<> engine_shutdown_;  // Lifecycle.EngineShutdown()
  ClrFallibleMethod<uint8_t> on_init_;   // Lifecycle.OnInit(byte isReload)
  ClrFallibleMethod<float> on_tick_;     // Lifecycle.OnTick(float dt)
  ClrFallibleMethod<> on_shutdown_;      // Lifecycle.OnShutdown()

  // Script loading (HotReloadManager entry points)
  ClrFallibleMethod<const uint8_t*, int32_t> load_scripts_;      // LoadScripts
  ClrFallibleMethod<> serialize_and_unload_;                     // SerializeAndUnload
  ClrFallibleMethod<const uint8_t*, int32_t> load_and_restore_;  // LoadAndRestore

  static constexpr std::string_view kLifecycleType = "Atlas.Core.Lifecycle, Atlas.Runtime";
  static constexpr std::string_view kHotReloadType =
      "Atlas.Hosting.HotReloadManager, Atlas.Runtime";

  void ResetAllMethods();
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_SCRIPT_ENGINE_H_
