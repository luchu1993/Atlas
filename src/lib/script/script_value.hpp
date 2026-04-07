#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace atlas
{

// 前向声明，避免与 script_object.hpp 循环依赖
class ScriptObject;

// ============================================================================
// ScriptValue — Type-erased value container for script <-> engine data exchange
// ============================================================================
//
// Covers the common types that cross the script boundary.
// Complex/custom types should use ScriptObject or raw bytes.

class ScriptValue
{
public:
    using Bytes = std::vector<std::byte>;
    using ObjectPtr = std::shared_ptr<ScriptObject>;

    ScriptValue() : data_(std::monostate{}) {}
    explicit ScriptValue(bool v) : data_(v) {}
    explicit ScriptValue(int64_t v) : data_(v) {}
    explicit ScriptValue(double v) : data_(v) {}
    explicit ScriptValue(std::string v) : data_(std::move(v)) {}
    explicit ScriptValue(Bytes v) : data_(std::move(v)) {}
    explicit ScriptValue(ObjectPtr v) : data_(std::move(v)) {}

    static auto from_int(int32_t v) -> ScriptValue { return ScriptValue(static_cast<int64_t>(v)); }
    static auto from_float(float v) -> ScriptValue { return ScriptValue(static_cast<double>(v)); }

    [[nodiscard]] auto is_none() const -> bool
    {
        return std::holds_alternative<std::monostate>(data_);
    }
    [[nodiscard]] auto is_bool() const -> bool { return std::holds_alternative<bool>(data_); }
    [[nodiscard]] auto is_int() const -> bool { return std::holds_alternative<int64_t>(data_); }
    [[nodiscard]] auto is_double() const -> bool { return std::holds_alternative<double>(data_); }
    [[nodiscard]] auto is_string() const -> bool
    {
        return std::holds_alternative<std::string>(data_);
    }
    [[nodiscard]] auto is_bytes() const -> bool { return std::holds_alternative<Bytes>(data_); }
    [[nodiscard]] auto is_object() const -> bool
    {
        return std::holds_alternative<ObjectPtr>(data_);
    }

    [[nodiscard]] auto as_bool() const -> bool { return std::get<bool>(data_); }
    [[nodiscard]] auto as_int() const -> int64_t { return std::get<int64_t>(data_); }
    [[nodiscard]] auto as_double() const -> double { return std::get<double>(data_); }
    [[nodiscard]] auto as_string() const -> const std::string&
    {
        return std::get<std::string>(data_);
    }
    [[nodiscard]] auto as_bytes() const -> const Bytes& { return std::get<Bytes>(data_); }
    [[nodiscard]] auto as_object() const -> const ObjectPtr& { return std::get<ObjectPtr>(data_); }

private:
    std::variant<std::monostate,  // None / null
                 bool, int64_t, double, std::string, Bytes, ObjectPtr>
        data_;
};

}  // namespace atlas
