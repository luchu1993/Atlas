#pragma once

#include "foundation/error.hpp"
#include "script/script_value.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

// ============================================================================
// ScriptObject — Language-agnostic script object interface
// ============================================================================
//
// Concrete implementations:
//   - ClrObject (.NET GCHandle wrapper)  [ScriptPhase 2]
//
// Thread safety: depends on the concrete implementation.

class ScriptObject
{
public:
    virtual ~ScriptObject() = default;

    [[nodiscard]] virtual auto is_none() const -> bool = 0;
    [[nodiscard]] virtual auto type_name() const -> std::string = 0;

    [[nodiscard]] virtual auto get_attr(std::string_view name) -> std::unique_ptr<ScriptObject> = 0;
    [[nodiscard]] virtual auto set_attr(std::string_view name, const ScriptValue& value)
        -> Result<void> = 0;

    [[nodiscard]] virtual auto as_int() const -> Result<int64_t> = 0;
    [[nodiscard]] virtual auto as_double() const -> Result<double> = 0;
    [[nodiscard]] virtual auto as_string() const -> Result<std::string> = 0;
    [[nodiscard]] virtual auto as_bool() const -> Result<bool> = 0;
    [[nodiscard]] virtual auto as_bytes() const -> Result<std::vector<std::byte>> = 0;

    [[nodiscard]] virtual auto is_callable() const -> bool = 0;
    [[nodiscard]] virtual auto call(std::span<const ScriptValue> args = {})
        -> Result<ScriptValue> = 0;

    [[nodiscard]] virtual auto to_debug_string() const -> std::string
    {
        return std::string(type_name());
    }
};

}  // namespace atlas
