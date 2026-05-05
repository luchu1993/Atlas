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

class ClrScriptEngine final : public ScriptEngine {
 public:
  struct Config {
    std::filesystem::path runtime_config_path;    // runtimeconfig.json
    std::filesystem::path runtime_assembly_path;  // script assembly (search root)
    std::optional<ClrBootstrapArgs> bootstrap_args;

    // Managed entry-point types the engine binds during Initialize. Each
    // field can be cleared to skip the corresponding binding set: the
    // desktop-client host passes empty strings for both because its
    // lifecycle and assembly-loading paths live outside Atlas.Runtime.
    // Server hosts leave the defaults in place.
    std::string lifecycle_type{"Atlas.Core.Lifecycle, Atlas.Runtime"};
    std::string hotreload_type{"Atlas.Hosting.HotReloadManager, Atlas.Runtime"};
  };

  [[nodiscard]] auto Configure(const Config& config) -> Result<void>;

  [[nodiscard]] auto Initialize() -> Result<void> override;
  void Finalize() override;
  [[nodiscard]] auto LoadModule(const std::filesystem::path& path) -> Result<void> override;
  void OnTick(float dt) override;
  void OnInit(bool is_reload) override;
  void OnShutdown() override;
  [[nodiscard]] auto CallFunction(std::string_view module_name, std::string_view function_name,
                                  std::span<const ScriptValue> args)
      -> Result<ScriptValue> override;
  [[nodiscard]] auto RuntimeName() const -> std::string_view override { return "CLR (CoreCLR)"; }

  [[nodiscard]] auto IsInitialized() const -> bool { return initialized_; }

  [[nodiscard]] auto CallHotReload(std::string_view method_name) -> Result<void>;

  [[nodiscard]] auto CallHotReload(std::string_view method_name,
                                   const std::filesystem::path& assembly_path) -> Result<void>;

  [[nodiscard]] auto Host() -> ClrHost& { return host_; }

 private:
  Config config_;
  ClrHost host_;
  bool configured_{false};
  bool initialized_{false};

  ClrFallibleMethod<> engine_init_;      // Lifecycle.EngineInit()
  ClrFallibleMethod<> engine_shutdown_;  // Lifecycle.EngineShutdown()
  ClrFallibleMethod<uint8_t> on_init_;   // Lifecycle.OnInit(byte isReload)
  ClrFallibleMethod<float> on_tick_;     // Lifecycle.OnTick(float dt)
  ClrFallibleMethod<> on_shutdown_;      // Lifecycle.OnShutdown()

  ClrFallibleMethod<const uint8_t*, int32_t> load_scripts_;      // LoadScripts
  ClrFallibleMethod<> serialize_and_unload_;                     // SerializeAndUnload
  ClrFallibleMethod<const uint8_t*, int32_t> load_and_restore_;  // LoadAndRestore

  void ResetAllMethods();
};

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_SCRIPT_ENGINE_H_
