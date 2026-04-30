#ifndef ATLAS_LIB_SCRIPT_SCRIPT_VALUE_H_
#define ATLAS_LIB_SCRIPT_SCRIPT_VALUE_H_

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace atlas {

class ScriptObject;

class ScriptValue {
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

  static auto FromInt(int32_t v) -> ScriptValue { return ScriptValue(static_cast<int64_t>(v)); }
  static auto FromFloat(float v) -> ScriptValue { return ScriptValue(static_cast<double>(v)); }

  [[nodiscard]] auto IsNone() const -> bool {
    return std::holds_alternative<std::monostate>(data_);
  }
  [[nodiscard]] auto IsBool() const -> bool { return std::holds_alternative<bool>(data_); }
  [[nodiscard]] auto IsInt() const -> bool { return std::holds_alternative<int64_t>(data_); }
  [[nodiscard]] auto IsDouble() const -> bool { return std::holds_alternative<double>(data_); }
  [[nodiscard]] auto IsString() const -> bool { return std::holds_alternative<std::string>(data_); }
  [[nodiscard]] auto IsBytes() const -> bool { return std::holds_alternative<Bytes>(data_); }
  [[nodiscard]] auto IsObject() const -> bool { return std::holds_alternative<ObjectPtr>(data_); }

  [[nodiscard]] auto AsBool() const -> bool { return std::get<bool>(data_); }
  [[nodiscard]] auto AsInt() const -> int64_t { return std::get<int64_t>(data_); }
  [[nodiscard]] auto AsDouble() const -> double { return std::get<double>(data_); }
  [[nodiscard]] auto AsString() const -> const std::string& { return std::get<std::string>(data_); }
  [[nodiscard]] auto AsBytes() const -> const Bytes& { return std::get<Bytes>(data_); }
  [[nodiscard]] auto AsObject() const -> const ObjectPtr& { return std::get<ObjectPtr>(data_); }

 private:
  std::variant<std::monostate,  // None / null
               bool, int64_t, double, std::string, Bytes, ObjectPtr>
      data_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SCRIPT_SCRIPT_VALUE_H_
