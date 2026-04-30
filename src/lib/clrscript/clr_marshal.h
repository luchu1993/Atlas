#ifndef ATLAS_LIB_CLRSCRIPT_CLR_MARSHAL_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_MARSHAL_H_

#include <climits>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "foundation/error.h"
#include "math/quaternion.h"
#include "math/vector3.h"
#include "script/script_value.h"

namespace atlas {

// These POD types cross the C++/C# boundary with zero-copy or one-copy
// semantics.  Each struct must have a matching [StructLayout(Sequential)]
// counterpart on the C# side with an identical binary layout.
// Pointer fields inside these structs point into C++ memory. The C# callee
//   must not store the pointer past the duration of the native call.  Use
//   ReadOnlySpan<byte>.ToArray() or string allocation to copy when needed.
// Pointer fields from C# come from pinned managed memory. The C++ callee must copy
//   the data before the pinning scope ends (i.e. before the native call returns).

struct ClrStringRef {
  const char* data;  // UTF-8, not null-terminated; may be null if length == 0
  int32_t length;    // byte count, not character count
};

struct ClrSpanRef {
  const std::byte* data;
  int32_t length;
};

struct ClrVector3 {
  float x, y, z;
};

struct ClrQuaternion {
  float x, y, z, w;
};

enum class ClrScriptValueType : int32_t {
  kNone = 0,
  kBool = 1,  // stored as uint8_t (0 or 1); do NOT use C# bool (4 bytes in marshal)
  kInt64 = 2,
  kDouble = 3,
  kString = 4,
  kBytes = 5,
  kObject = 6,

  // Range [100, 199] is reserved for future composite types.
  // Extend here to preserve ABI compatibility with existing values.
  // Array    = 100,
  // Map      = 101,
  // EntityRef = 102,
};

struct ClrScriptValue {
  ClrScriptValueType type;  // offset 0
  int32_t pad{};
  union {
    uint8_t bool_val;
    int64_t int_val;
    double double_val;
    ClrStringRef string_val;
    ClrSpanRef bytes_val;
    void* object_val;
  };
};

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
static_assert(offsetof(ClrScriptValue, pad) == 4);
static_assert(offsetof(ClrScriptValue, bool_val) == 8);
static_assert(offsetof(ClrScriptValue, int_val) == 8);
static_assert(offsetof(ClrScriptValue, double_val) == 8);
static_assert(offsetof(ClrScriptValue, string_val) == 8);
static_assert(offsetof(ClrScriptValue, bytes_val) == 8);
static_assert(offsetof(ClrScriptValue, object_val) == 8);

#endif  // x64

static_assert(sizeof(bool) == 1, "Unexpected bool size; adjust bool marshal strategy");

namespace clr_marshal {

[[nodiscard]] inline auto ToStringRef(std::string_view sv) -> ClrStringRef {
  ATLAS_ASSERT(sv.size() <= static_cast<std::size_t>(INT32_MAX));
  return {sv.data(), static_cast<int32_t>(sv.size())};
}

[[nodiscard]] auto FromStringRef(ClrStringRef ref) -> std::string;

[[nodiscard]] inline auto ToSpanRef(std::span<const std::byte> bytes) -> ClrSpanRef {
  ATLAS_ASSERT(bytes.size() <= static_cast<std::size_t>(INT32_MAX));
  return {bytes.data(), static_cast<int32_t>(bytes.size())};
}

[[nodiscard]] inline auto ToVector3(const math::Vector3& v) -> ClrVector3 {
  return {v.x, v.y, v.z};
}

[[nodiscard]] inline auto FromVector3(ClrVector3 v) -> math::Vector3 {
  return {v.x, v.y, v.z};
}

[[nodiscard]] inline auto ToQuaternion(const math::Quaternion& q) -> ClrQuaternion {
  return {q.x, q.y, q.z, q.w};
}

[[nodiscard]] inline auto FromQuaternion(ClrQuaternion q) -> math::Quaternion {
  return {q.x, q.y, q.z, q.w};
}

// to_script_value: zero-copy for String/Bytes (result holds a pointer into sv);
//   sv must outlive the returned ClrScriptValue and any C# call using it.
// from_script_value: copies String/Bytes data onto the C++ heap.

[[nodiscard]] auto ToScriptValue(const ScriptValue& sv) -> ClrScriptValue;
[[nodiscard]] auto FromScriptValue(const ClrScriptValue& cv) -> ScriptValue;

}  // namespace clr_marshal

}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_MARSHAL_H_
