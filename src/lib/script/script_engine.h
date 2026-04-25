#ifndef ATLAS_LIB_SCRIPT_SCRIPT_ENGINE_H_
#define ATLAS_LIB_SCRIPT_SCRIPT_ENGINE_H_

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "foundation/error.h"
#include "script/script_object.h"
#include "script/script_value.h"

namespace atlas {

// ============================================================================
// ScriptEngine — Language-agnostic script runtime interface
// ============================================================================
//
// Concrete implementations:
//   - ClrScriptEngine (.NET / CoreCLR)
//
// Lifecycle: initialize() -> [load_module()* -> on_tick()*] -> finalize()

class ScriptEngine {
 public:
  virtual ~ScriptEngine() = default;

  // Initialize the scripting runtime.
  [[nodiscard]] virtual auto Initialize() -> Result<void> = 0;

  // Shut down the scripting runtime and release all resources.
  virtual void Finalize() = 0;

  // Load a script module/assembly from the given path.
  [[nodiscard]] virtual auto LoadModule(const std::filesystem::path& path) -> Result<void> = 0;

  // Called each server tick.
  virtual void OnTick(float dt) = 0;

  // Called on server initialization (after all modules loaded).
  virtual void OnInit(bool is_reload) = 0;

  // Called before shutdown.
  virtual void OnShutdown() = 0;

  // Call a global function by name.
  [[nodiscard]] virtual auto CallFunction(std::string_view module_name,
                                          std::string_view function_name,
                                          std::span<const ScriptValue> args)
      -> Result<ScriptValue> = 0;

  // Runtime name for diagnostics ("CLR", "Python", etc.)
  [[nodiscard]] virtual auto RuntimeName() const -> std::string_view = 0;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SCRIPT_SCRIPT_ENGINE_H_
