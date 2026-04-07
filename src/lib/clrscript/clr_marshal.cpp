#include "clrscript/clr_marshal.hpp"

namespace atlas::clr_marshal
{

// ---- String -----------------------------------------------------------------

auto from_string_ref(ClrStringRef ref) -> std::string
{
    if (ref.data == nullptr || ref.length <= 0)
        return {};
    return {ref.data, static_cast<std::size_t>(ref.length)};
}

// ---- ScriptValue → ClrScriptValue ------------------------------------------
//
// String and Bytes cases return a pointer into the ScriptValue's internal
// storage.  The caller must ensure the ScriptValue outlives the returned
// ClrScriptValue and any C# call that consumes it.

auto to_script_value(const ScriptValue& sv) -> ClrScriptValue
{
    ClrScriptValue cv{};

    if (sv.is_none())
    {
        cv.type = ClrScriptValueType::None;
    }
    else if (sv.is_bool())
    {
        cv.type = ClrScriptValueType::Bool;
        cv.bool_val = sv.as_bool() ? uint8_t{1} : uint8_t{0};
    }
    else if (sv.is_int())
    {
        cv.type = ClrScriptValueType::Int64;
        cv.int_val = sv.as_int();
    }
    else if (sv.is_double())
    {
        cv.type = ClrScriptValueType::Double;
        cv.double_val = sv.as_double();
    }
    else if (sv.is_string())
    {
        const auto& s = sv.as_string();
        cv.type = ClrScriptValueType::String;
        cv.string_val = {s.data(), static_cast<int32_t>(s.size())};
    }
    else if (sv.is_bytes())
    {
        const auto& b = sv.as_bytes();
        cv.type = ClrScriptValueType::Bytes;
        cv.bytes_val = {b.data(), static_cast<int32_t>(b.size())};
    }
    else if (sv.is_object())
    {
        // Phase 2.2: ClrObject stores a GCHandle as void*.  Until ClrObject
        // is implemented, pass the raw ScriptObject* as an opaque handle.
        // This value is informational only — do not dereference on the C# side
        // without the full GCHandle machinery in place.
        cv.type = ClrScriptValueType::Object;
        cv.object_val = sv.as_object().get();
    }

    return cv;
}

// ---- ClrScriptValue → ScriptValue ------------------------------------------
//
// String and Bytes cases copy the data from the ClrScriptValue into C++ heap
// storage.  The returned ScriptValue is fully owned and independent of the
// source ClrScriptValue.

auto from_script_value(const ClrScriptValue& cv) -> ScriptValue
{
    switch (cv.type)
    {
        case ClrScriptValueType::None:
            return ScriptValue{};

        case ClrScriptValueType::Bool:
            return ScriptValue{cv.bool_val != uint8_t{0}};

        case ClrScriptValueType::Int64:
            return ScriptValue{cv.int_val};

        case ClrScriptValueType::Double:
            return ScriptValue{cv.double_val};

        case ClrScriptValueType::String:
        {
            const auto& s = cv.string_val;
            if (s.data == nullptr || s.length <= 0)
                return ScriptValue{std::string{}};
            return ScriptValue{std::string{s.data, static_cast<std::size_t>(s.length)}};
        }

        case ClrScriptValueType::Bytes:
        {
            const auto& b = cv.bytes_val;
            if (b.data == nullptr || b.length <= 0)
                return ScriptValue{ScriptValue::Bytes{}};
            ScriptValue::Bytes bytes(b.data, b.data + b.length);
            return ScriptValue{std::move(bytes)};
        }

        case ClrScriptValueType::Object:
            // Phase 2.2: reconstruct a ClrObject from the GCHandle stored in
            // cv.object_val.  Until ClrObject exists, return None to avoid
            // undefined behaviour from an unmanaged raw pointer.
            return ScriptValue{};

        default:
            return ScriptValue{};
    }
}

}  // namespace atlas::clr_marshal
