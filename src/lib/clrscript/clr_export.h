#ifndef ATLAS_LIB_CLRSCRIPT_CLR_EXPORT_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_EXPORT_H_

// ATLAS_ENGINE_EXPORTS is defined when compiling atlas_engine.dll/.so.
// Other translation units see the dllimport / default-hidden variant.

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

// AtlasGetAbiVersion lets managed hosts and tests detect incompatible
// interop surfaces early.

// Bump this integer whenever the C++/C# native API table or shared blittable
// type layout changes.

#include <cstdint>

namespace atlas {
inline constexpr uint32_t kAtlasAbiVersion = 5;
}  // namespace atlas

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_EXPORT_H_
