#pragma once

#include "foundation/error.hpp"
#include "script/script_object.hpp"
#include "script/script_value.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace atlas
{

// ============================================================================
// ScriptEngine — Language-agnostic script runtime interface
// ============================================================================
//
// Concrete implementations:
//   - ClrScriptEngine (.NET / CoreCLR)  [ScriptPhase 1-3]
//
// Lifecycle: initialize() -> [load_module()* -> on_tick()*] -> finalize()

class ScriptEngine
{
public:
    virtual ~ScriptEngine() = default;

    // Initialize the scripting runtime.
    [[nodiscard]] virtual auto initialize() -> Result<void> = 0;

    // Shut down the scripting runtime and release all resources.
    virtual void finalize() = 0;

    // Load a script module/assembly from the given path.
    [[nodiscard]] virtual auto load_module(const std::filesystem::path& path) -> Result<void> = 0;

    // Called each server tick.
    virtual void on_tick(float dt) = 0;

    // Called on server initialization (after all modules loaded).
    virtual void on_init(bool is_reload = false) = 0;

    // Called before shutdown.
    virtual void on_shutdown() = 0;

    // Call a global function by name.
    [[nodiscard]] virtual auto call_function(std::string_view module_name,
                                             std::string_view function_name,
                                             std::span<const ScriptValue> args)
        -> Result<ScriptValue> = 0;

    // Runtime name for diagnostics ("CLR", "Python", etc.)
    [[nodiscard]] virtual auto runtime_name() const -> std::string_view = 0;
};

}  // namespace atlas
