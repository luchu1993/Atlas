#pragma once

#include "clrscript/clr_export.hpp"
#include "clrscript/clr_native_api_defs.hpp"

#include <cstddef>
#include <cstdint>

// ============================================================================
// atlas_* C-linkage export function declarations
// ============================================================================
//
// Generated from ATLAS_NATIVE_API_TABLE in clr_native_api_defs.hpp.
// To add a new export, edit clr_native_api_defs.hpp ONLY — this file and
// clr_native_api.cpp update automatically.
//
// atlas_get_abi_version() is declared separately (not in the table) because it
// does not delegate to INativeApiProvider.

#define X(ret, name, params, call) ATLAS_NATIVE_API ret atlas_##name params;
ATLAS_NATIVE_API_TABLE(X)
#undef X

// ---- ABI version ------------------------------------------------------------
// C# startup code calls this to verify the native/managed struct layouts match.
// Returns atlas::kAtlasAbiVersion.

ATLAS_NATIVE_API uint32_t atlas_get_abi_version();
