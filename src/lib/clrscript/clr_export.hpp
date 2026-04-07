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

#if defined(_WIN32)
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
