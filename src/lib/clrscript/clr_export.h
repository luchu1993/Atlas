#ifndef ATLAS_LIB_CLRSCRIPT_CLR_EXPORT_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_EXPORT_H_

// ATLAS_ENGINE_EXPORTS must be defined when compiling atlas_engine.dll/.so
// (set by the atlas_engine CMake target). All other translation units see
// the dllimport / default-hidden variant.

#if ATLAS_PLATFORM_WINDOWS
#ifdef ATLAS_ENGINE_EXPORTS
#define ATLAS_EXPORT __declspec(dllexport)
#else
#define ATLAS_EXPORT __declspec(dllimport)
#endif
#else
#define ATLAS_EXPORT __attribute__((visibility("default")))
#endif

#define ATLAS_NATIVE_API extern "C" ATLAS_EXPORT

// C# managed code calls AtlasGetAbiVersion() at startup and compares the
// result against its own compile-time constant.  A mismatch means the native
// and managed sides were built against different struct definitions, which
// causes silent data corruption and must be caught early.

// Bump this integer whenever ClrScriptValue, ClrStringRef, ClrSpanRef, or any
// other blittable type shared across the C++/C# boundary changes its layout.

#include <cstdint>

namespace atlas {
inline constexpr uint32_t kAtlasAbiVersion = 1;
}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_EXPORT_H_
