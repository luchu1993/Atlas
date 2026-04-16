#include "clrscript/clr_marshal.h"

#include "foundation/log.h"

namespace atlas::clr_marshal {

// ---- String -----------------------------------------------------------------

auto FromStringRef(ClrStringRef ref) -> std::string {
  if (ref.data == nullptr || ref.length <= 0) return {};
  return {ref.data, static_cast<std::size_t>(ref.length)};
}

// ---- ScriptValue → ClrScriptValue ------------------------------------------
//
// String and Bytes cases return a pointer into the ScriptValue's internal
// storage.  The caller must ensure the ScriptValue outlives the returned
// ClrScriptValue and any C# call that consumes it.

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
    // Phase 2.2: Implement GCHandle machinery (ClrObjectRegistry) before
    // enabling Object transfer.  Passing a raw ScriptObject* is unsafe:
    // the shared_ptr may expire before C# uses the pointer, causing a
    // use-after-free.  Return None + log a warning until Phase 2.2.
    ATLAS_LOG_WARNING(
        "to_script_value: Object type not supported until Phase 2.2 "
        "(GCHandle), converting to None");
    cv.type = ClrScriptValueType::kNone;
  }

  return cv;
}

// ---- ClrScriptValue → ScriptValue ------------------------------------------
//
// String and Bytes cases copy the data from the ClrScriptValue into C++ heap
// storage.  The returned ScriptValue is fully owned and independent of the
// source ClrScriptValue.

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
      // Phase 2.2: reconstruct a ClrObject from the GCHandle stored in
      // cv.object_val.  Until ClrObject exists, return None to avoid
      // undefined behaviour from an unmanaged raw pointer.
      return ScriptValue{};

    default:
      return ScriptValue{};
  }
}

}  // namespace atlas::clr_marshal
