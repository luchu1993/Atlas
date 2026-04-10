#pragma once

#include "foundation/error.hpp"
#include "math/quaternion.hpp"
#include "math/vector3.hpp"
#include "script/script_value.hpp"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace atlas
{

// ============================================================================
// Blittable transfer structures
// ============================================================================
//
// These POD types cross the C++/C# boundary with zero-copy or one-copy
// semantics.  Each struct must have a matching [StructLayout(Sequential)]
// counterpart on the C# side with an identical binary layout.
//
// Lifetimes (C++ → C# direction):
//   Pointer fields inside these structs point into C++ memory.  The C# callee
//   must not store the pointer past the duration of the native call.  Use
//   ReadOnlySpan<byte>.ToArray() or string allocation to copy when needed.
//
// Lifetimes (C# → C++ direction):
//   Pointer fields come from pinned managed memory.  The C++ callee must copy
//   the data before the pinning scope ends (i.e. before the native call returns).

// ---- String -----------------------------------------------------------------
//
// Carries a UTF-8 string as a non-owning view.
// C# counterpart: unsafe struct NativeStringRef { byte* Data; int Length; }
//
// Layout (x64):
//   offset 0 : data   — 8 bytes (pointer)
//   offset 8 : length — 4 bytes (int32_t)
//   offset 12: [4 bytes implicit trailing padding]
//   sizeof   = 16

struct ClrStringRef
{
    const char* data;  // UTF-8, not null-terminated; may be null if length == 0
    int32_t length;    // byte count, not character count
};

// ---- Byte span --------------------------------------------------------------
//
// Carries an arbitrary byte buffer as a non-owning view.
// C# counterpart: unsafe struct NativeSpanRef { byte* Data; int Length; }
//
// Layout (x64): identical to ClrStringRef, sizeof = 16

struct ClrSpanRef
{
    const std::byte* data;
    int32_t length;
};

// ---- Math types -------------------------------------------------------------
//
// Blittable 1:1 with atlas::math::Vector3 / atlas::math::Quaternion.
// C# counterpart: [StructLayout(Sequential)] struct Vector3 { float X, Y, Z; }
//
// sizeof(ClrVector3)    = 12, alignof = 4
// sizeof(ClrQuaternion) = 16, alignof = 4

struct ClrVector3
{
    float x, y, z;
};

struct ClrQuaternion
{
    float x, y, z, w;
};

// ---- ScriptValue ------------------------------------------------------------
//
// Tagged-union representation of ScriptValue suitable for crossing the
// C++/C# boundary.  C# uses [StructLayout(LayoutKind.Explicit, Size=24)].
//
// Layout (x64):
//   offset  0 : type  — int32_t  (4 bytes)
//   offset  4 : _pad  — int32_t  (4 bytes, explicit padding for union alignment)
//   offset  8 : union — 16 bytes (largest member: ClrStringRef / ClrSpanRef)
//   sizeof    = 24

enum class ClrScriptValueType : int32_t
{
    None = 0,
    Bool = 1,  // stored as uint8_t (0 or 1); do NOT use C# bool (4 bytes in marshal)
    Int64 = 2,
    Double = 3,
    String = 4,  // ClrStringRef — points into C++ std::string storage
    Bytes = 5,   // ClrSpanRef   — points into C++ std::vector<byte> storage
    Object = 6,  // void*        — opaque GCHandle; ownership managed by ClrObject (Phase 2.2)

    // Range [100, 199] is reserved for future composite types.
    // Extend here to preserve ABI compatibility with existing values.
    // Array    = 100,
    // Map      = 101,
    // EntityRef = 102,
};

struct ClrScriptValue
{
    ClrScriptValueType type;  // offset 0
    int32_t _pad{};           // offset 4 — explicit padding; must be zero
    union
    {
        uint8_t bool_val;         // offset 8 — 0 = false, 1 = true
        int64_t int_val;          // offset 8
        double double_val;        // offset 8
        ClrStringRef string_val;  // offset 8 — non-owning view into std::string
        ClrSpanRef bytes_val;     // offset 8 — non-owning view into vector<byte>
        void* object_val;         // offset 8 — GCHandle (Phase 2.2)
    };
};

// ============================================================================
// Layout assertions (x64 targets only)
// ============================================================================

#if defined(_WIN64) || defined(__LP64__)

static_assert(sizeof(ClrStringRef) == 16, "ClrStringRef layout mismatch with C# NativeStringRef");
static_assert(offsetof(ClrStringRef, data) == 0);
static_assert(offsetof(ClrStringRef, length) == 8);

static_assert(sizeof(ClrSpanRef) == 16, "ClrSpanRef layout mismatch with C# NativeSpanRef");
static_assert(offsetof(ClrSpanRef, data) == 0);
static_assert(offsetof(ClrSpanRef, length) == 8);

static_assert(sizeof(ClrVector3) == 12, "ClrVector3 layout mismatch with C# Vector3");
static_assert(sizeof(ClrQuaternion) == 16, "ClrQuaternion layout mismatch with C# Quaternion");

static_assert(sizeof(ClrScriptValue) == 24,
              "ClrScriptValue layout mismatch with C# NativeScriptValue");
static_assert(offsetof(ClrScriptValue, type) == 0);
static_assert(offsetof(ClrScriptValue, _pad) == 4);
static_assert(offsetof(ClrScriptValue, bool_val) == 8);
static_assert(offsetof(ClrScriptValue, int_val) == 8);
static_assert(offsetof(ClrScriptValue, double_val) == 8);
static_assert(offsetof(ClrScriptValue, string_val) == 8);
static_assert(offsetof(ClrScriptValue, bytes_val) == 8);
static_assert(offsetof(ClrScriptValue, object_val) == 8);

#endif  // x64

// C++ bool must be 1 byte so uint8_t round-trips correctly.
static_assert(sizeof(bool) == 1, "Unexpected bool size; adjust bool marshal strategy");

// ============================================================================
// Marshal helpers — namespace clr_marshal
// ============================================================================
//
// to_*  : C++ type  → blittable transfer type   (zero-copy; caller owns memory)
// from_*: blittable transfer type → C++ type    (copies data onto C++ heap)

namespace clr_marshal
{

// ---- String -----------------------------------------------------------------

// Zero-copy: returns a non-owning view into sv's storage.
// The returned ClrStringRef is valid only while sv is alive.
[[nodiscard]] inline auto to_string_ref(std::string_view sv) -> ClrStringRef
{
    ATLAS_ASSERT(sv.size() <= static_cast<std::size_t>(INT32_MAX));
    return {sv.data(), static_cast<int32_t>(sv.size())};
}

// Copies the UTF-8 bytes into a new std::string.
[[nodiscard]] auto from_string_ref(ClrStringRef ref) -> std::string;

// ---- Byte span --------------------------------------------------------------

// Zero-copy: returns a non-owning view into bytes' storage.
[[nodiscard]] inline auto to_span_ref(std::span<const std::byte> bytes) -> ClrSpanRef
{
    ATLAS_ASSERT(bytes.size() <= static_cast<std::size_t>(INT32_MAX));
    return {bytes.data(), static_cast<int32_t>(bytes.size())};
}

// ---- Math -------------------------------------------------------------------

[[nodiscard]] inline auto to_vector3(const math::Vector3& v) -> ClrVector3
{
    return {v.x, v.y, v.z};
}

[[nodiscard]] inline auto from_vector3(ClrVector3 v) -> math::Vector3
{
    return {v.x, v.y, v.z};
}

[[nodiscard]] inline auto to_quaternion(const math::Quaternion& q) -> ClrQuaternion
{
    return {q.x, q.y, q.z, q.w};
}

[[nodiscard]] inline auto from_quaternion(ClrQuaternion q) -> math::Quaternion
{
    return {q.x, q.y, q.z, q.w};
}

// ---- ScriptValue ------------------------------------------------------------
//
// to_script_value: zero-copy for String/Bytes (result holds a pointer into sv);
//   sv must outlive the returned ClrScriptValue and any C# call using it.
//
// from_script_value: copies String/Bytes data onto the C++ heap.
//   Object case returns ScriptValue{} (GCHandle reconstruction is Phase 2.2).

[[nodiscard]] auto to_script_value(const ScriptValue& sv) -> ClrScriptValue;
[[nodiscard]] auto from_script_value(const ClrScriptValue& cv) -> ScriptValue;

}  // namespace clr_marshal

}  // namespace atlas
