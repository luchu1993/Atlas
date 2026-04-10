#pragma once

#include "clrscript/clr_bootstrap.hpp"
#include "clrscript/clr_host.hpp"
#include "clrscript/clr_invoke.hpp"
#include "foundation/error.hpp"
#include "script/script_engine.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace atlas
{

// ============================================================================
// ClrScriptEngine — ScriptEngine implementation backed by .NET CoreCLR
// ============================================================================
//
// Lifecycle:
//   1. configure()   — set paths for runtimeconfig.json and Atlas.Runtime.dll
//   2. initialize()  — start CLR, bootstrap, bind lifecycle methods
//   3. load_module() — load user game-script assemblies (optional)
//   4. on_init()     — trigger C# entity initialization
//   5. on_tick()     — per-frame update (called from main loop)
//   6. on_shutdown() — trigger C# entity cleanup
//   7. finalize()    — shut down CLR
//
// All lifecycle methods use the "return 0 = ok, return -1 = error" convention.
// Errors from C# are read via the TLS error bridge (clr_error.hpp).

class ClrScriptEngine final : public ScriptEngine
{
public:
    struct Config
    {
        std::filesystem::path runtime_config_path;    // runtimeconfig.json
        std::filesystem::path runtime_assembly_path;  // Atlas.Runtime.dll
    };

    [[nodiscard]] auto configure(const Config& config) -> Result<void>;

    // -- ScriptEngine interface -----------------------------------------------

    [[nodiscard]] auto initialize() -> Result<void> override;
    void finalize() override;
    [[nodiscard]] auto load_module(const std::filesystem::path& path) -> Result<void> override;
    void on_tick(float dt) override;
    void on_init(bool is_reload = false) override;
    void on_shutdown() override;
    [[nodiscard]] auto call_function(std::string_view module_name, std::string_view function_name,
                                     std::span<const ScriptValue> args)
        -> Result<ScriptValue> override;
    [[nodiscard]] auto runtime_name() const -> std::string_view override { return "CLR (.NET 9)"; }

    [[nodiscard]] auto is_initialized() const -> bool { return initialized_; }

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

    static constexpr std::string_view kLifecycleType = "Atlas.Core.Lifecycle, Atlas.Runtime";

    void reset_all_methods();
};

}  // namespace atlas
