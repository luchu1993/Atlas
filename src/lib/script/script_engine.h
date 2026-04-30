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

class ScriptEngine {
 public:
  virtual ~ScriptEngine() = default;

  [[nodiscard]] virtual auto Initialize() -> Result<void> = 0;

  virtual void Finalize() = 0;

  [[nodiscard]] virtual auto LoadModule(const std::filesystem::path& path) -> Result<void> = 0;

  virtual void OnTick(float dt) = 0;

  virtual void OnInit(bool is_reload) = 0;

  virtual void OnShutdown() = 0;

  [[nodiscard]] virtual auto CallFunction(std::string_view module_name,
                                          std::string_view function_name,
                                          std::span<const ScriptValue> args)
      -> Result<ScriptValue> = 0;

  [[nodiscard]] virtual auto RuntimeName() const -> std::string_view = 0;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SCRIPT_SCRIPT_ENGINE_H_
