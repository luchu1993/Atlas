#pragma once

// ============================================================================
// ATLAS_EXPORT / ATLAS_NATIVE_API — shared library symbol visibility
// ============================================================================
//
// ATLAS_ENGINE_EXPORTS must be defined when compiling atlas_engine.dll/.so
// (set by the atlas_engine CMake target). All other translation units see
// the dllimport / default-hidden variant.
//
// Usage:
//   ATLAS_NATIVE_API void atlas_log_message(int32_t level, const char* msg, int32_t len);

#if ATLAS_PLATFORM_WINDOWS
#ifdef ATLAS_ENGINE_EXPORTS
#define ATLAS_EXPORT __declspec(dllexport)
#else
#define ATLAS_EXPORT __declspec(dllimport)
#endif
#else
#define ATLAS_EXPORT __attribute__((visibility("default")))
#endif

// All atlas_* C-linkage export functions use this decorator.
#define ATLAS_NATIVE_API extern "C" ATLAS_EXPORT

// ============================================================================
// ABI version — increment whenever blittable struct layouts change
// ============================================================================
//
// C# managed code calls atlas_get_abi_version() at startup and compares the
// result against its own compile-time constant.  A mismatch means the native
// and managed sides were built against different struct definitions, which
// causes silent data corruption and must be caught early.
//
// Bump this integer whenever ClrScriptValue, ClrStringRef, ClrSpanRef, or any
// other blittable type shared across the C++/C# boundary changes its layout.

#include <cstdint>

namespace atlas
{
inline constexpr uint32_t kAtlasAbiVersion = 1;
}  // namespace atlas
