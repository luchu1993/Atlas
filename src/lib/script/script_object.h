#ifndef ATLAS_LIB_SCRIPT_SCRIPT_OBJECT_H_
#define ATLAS_LIB_SCRIPT_SCRIPT_OBJECT_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/error.h"
#include "script/script_value.h"

namespace atlas {

// ============================================================================
// ScriptObject — Language-agnostic script object interface
// ============================================================================
//
// Concrete implementations:
//   - ClrObject (.NET GCHandle wrapper)  [ScriptPhase 2]
//
// Thread safety: depends on the concrete implementation.

class ScriptObject {
 public:
  virtual ~ScriptObject() = default;

  [[nodiscard]] virtual auto IsNone() const -> bool = 0;
  [[nodiscard]] virtual auto TypeName() const -> std::string = 0;

  [[nodiscard]] virtual auto GetAttr(std::string_view name) -> std::unique_ptr<ScriptObject> = 0;
  [[nodiscard]] virtual auto SetAttr(std::string_view name, const ScriptValue& value)
      -> Result<void> = 0;

  [[nodiscard]] virtual auto AsInt() const -> Result<int64_t> = 0;
  [[nodiscard]] virtual auto AsDouble() const -> Result<double> = 0;
  [[nodiscard]] virtual auto AsString() const -> Result<std::string> = 0;
  [[nodiscard]] virtual auto AsBool() const -> Result<bool> = 0;
  [[nodiscard]] virtual auto AsBytes() const -> Result<std::vector<std::byte>> = 0;

  [[nodiscard]] virtual auto IsCallable() const -> bool = 0;
  [[nodiscard]] virtual auto Call(std::span<const ScriptValue> args = {})
      -> Result<ScriptValue> = 0;

  [[nodiscard]] virtual auto ToDebugString() const -> std::string {
    return std::string(TypeName());
  }
};

}  // namespace atlas

#endif  // ATLAS_LIB_SCRIPT_SCRIPT_OBJECT_H_
