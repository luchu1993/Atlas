#ifndef ATLAS_LIB_DB_DB_EXPORT_H_
#define ATLAS_LIB_DB_DB_EXPORT_H_

// ============================================================================
// ATLAS_DB_EXPORT — shared library symbol visibility for database plugins
// ============================================================================
//
// ATLAS_DB_BACKEND_EXPORTS must be defined when compiling a database backend
// plugin DLL/SO (set by the plugin CMake target).
//
// Each backend plugin exports a single C-linkage factory function:
//
//   ATLAS_DB_BACKEND_API atlas::IDatabase* AtlasCreateDatabase();
//
// The returned pointer is heap-allocated; ownership transfers to the caller.

#include "platform/platform_config.h"

#if ATLAS_PLATFORM_WINDOWS
#ifdef ATLAS_DB_BACKEND_EXPORTS
#define ATLAS_DB_EXPORT __declspec(dllexport)
#else
#define ATLAS_DB_EXPORT __declspec(dllimport)
#endif
#else
#define ATLAS_DB_EXPORT __attribute__((visibility("default")))
#endif

#define ATLAS_DB_BACKEND_API extern "C" ATLAS_DB_EXPORT

#endif  // ATLAS_LIB_DB_DB_EXPORT_H_
