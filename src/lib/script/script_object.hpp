#pragma once

#include "foundation/error.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace atlas
{

// ============================================================================
// ScriptObject — Language-agnostic script value interface
// ============================================================================
//
// Concrete implementations: PyScriptObject (Python 3).
//
// Thread safety: depends on the concrete implementation.

class ScriptObject
{
public:
    virtual ~ScriptObject() = default;

    [[nodiscard]] virtual auto is_none() const -> bool = 0;
    [[nodiscard]] virtual auto type_name() const -> std::string = 0;

    [[nodiscard]] virtual auto get_attr(std::string_view name) -> std::unique_ptr<ScriptObject> = 0;

    [[nodiscard]] virtual auto as_int() const -> Result<int64_t> = 0;
    [[nodiscard]] virtual auto as_double() const -> Result<double> = 0;
    [[nodiscard]] virtual auto as_string() const -> Result<std::string> = 0;
    [[nodiscard]] virtual auto as_bool() const -> Result<bool> = 0;
};

}  // namespace atlas
