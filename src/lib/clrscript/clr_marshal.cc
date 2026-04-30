#include "clrscript/clr_marshal.h"

#include "foundation/log.h"

namespace atlas::clr_marshal {

auto FromStringRef(ClrStringRef ref) -> std::string {
  if (ref.data == nullptr || ref.length <= 0) return {};
  return {ref.data, static_cast<std::size_t>(ref.length)};
}

auto ToScriptValue(const ScriptValue& sv) -> ClrScriptValue {
  ClrScriptValue cv{};

  if (sv.IsNone()) {
    cv.type = ClrScriptValueType::kNone;
  } else if (sv.IsBool()) {
    cv.type = ClrScriptValueType::kBool;
    cv.bool_val = sv.AsBool() ? uint8_t{1} : uint8_t{0};
  } else if (sv.IsInt()) {
    cv.type = ClrScriptValueType::kInt64;
    cv.int_val = sv.AsInt();
  } else if (sv.IsDouble()) {
    cv.type = ClrScriptValueType::kDouble;
    cv.double_val = sv.AsDouble();
  } else if (sv.IsString()) {
    const auto& s = sv.AsString();
    ATLAS_ASSERT(s.size() <= static_cast<std::size_t>(INT32_MAX));
    cv.type = ClrScriptValueType::kString;
    cv.string_val = {s.data(), static_cast<int32_t>(s.size())};
  } else if (sv.IsBytes()) {
    const auto& b = sv.AsBytes();
    cv.type = ClrScriptValueType::kBytes;
    cv.bytes_val = {b.data(), static_cast<int32_t>(b.size())};
  } else if (sv.IsObject()) {
    ATLAS_LOG_WARNING("to_script_value: Object type not yet supported, converting to None");
    cv.type = ClrScriptValueType::kNone;
  }

  return cv;
}

auto FromScriptValue(const ClrScriptValue& cv) -> ScriptValue {
  switch (cv.type) {
    case ClrScriptValueType::kNone:
      return ScriptValue{};

    case ClrScriptValueType::kBool:
      return ScriptValue{cv.bool_val != uint8_t{0}};

    case ClrScriptValueType::kInt64:
      return ScriptValue{cv.int_val};

    case ClrScriptValueType::kDouble:
      return ScriptValue{cv.double_val};

    case ClrScriptValueType::kString: {
      const auto& s = cv.string_val;
      if (s.data == nullptr || s.length <= 0) return ScriptValue{std::string{}};
      return ScriptValue{std::string{s.data, static_cast<std::size_t>(s.length)}};
    }

    case ClrScriptValueType::kBytes: {
      const auto& b = cv.bytes_val;
      if (b.data == nullptr || b.length <= 0) return ScriptValue{ScriptValue::Bytes{}};
      ScriptValue::Bytes bytes(b.data, b.data + b.length);
      return ScriptValue{std::move(bytes)};
    }

    case ClrScriptValueType::kObject:
      return ScriptValue{};

    default:
      return ScriptValue{};
  }
}

}  // namespace atlas::clr_marshal
