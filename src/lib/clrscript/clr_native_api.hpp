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

// ---- Provider registration --------------------------------------------------
// Register the INativeApiProvider for this process so that atlas_* functions
// can delegate to the correct implementation.  Must be called once before
// ClrHost::initialize().
//
// provider must point to an atlas::INativeApiProvider instance.
// Exported so that the calling process can set the provider even when it
// has loaded atlas_engine as a shared library (avoiding the duplicate-global
// problem that would arise from separate static linkage of atlas_clrscript).

ATLAS_NATIVE_API void atlas_set_native_api_provider(void* provider);

// ---- Error bridge query / write (DLL-side) ----------------------------------
// These functions read/write the TLS error buffer INSIDE atlas_engine.dll.
// Processes that also statically link atlas_clrscript have a SEPARATE copy of
// the TLS buffer.  Use these exports to ensure both C# (via clr_error_set_fn)
// and C++ (via atlas_has_clr_error / atlas_read_clr_error / atlas_clear_clr_error)
// access the SAME buffer inside the DLL.
//
// Intended use (e.g. in test drivers):
//   1. Obtain the DLL's clr_error_set address via atlas_get_clr_error_set_fn().
//   2. Pass it in ClrBootstrapArgs::error_set so C# writes into the DLL buffer.
//   3. Query the DLL buffer via atlas_has_clr_error / atlas_read_clr_error.

// Returns function pointers to the DLL's TLS error functions.
// Cast each result to the indicated function-pointer type before calling.
//   atlas_get_clr_error_set_fn   → void(*)(int32_t, const char*, int32_t)
//   atlas_get_clr_error_clear_fn → void(*)()
//   atlas_get_clr_error_get_code_fn → int32_t(*)()
ATLAS_NATIVE_API void* atlas_get_clr_error_set_fn();
ATLAS_NATIVE_API void* atlas_get_clr_error_clear_fn();
ATLAS_NATIVE_API void* atlas_get_clr_error_get_code_fn();

// Returns 1 if the current thread has an unread CLR error, 0 otherwise.
ATLAS_NATIVE_API int32_t atlas_has_clr_error();

// Copies the raw error message into buf (up to buf_len bytes, not null-terminated).
// Returns the error HResult.  Clears the buffer.
// If buf is null or buf_len <= 0, just clears and returns the code.
ATLAS_NATIVE_API int32_t atlas_read_clr_error(char* buf, int32_t buf_len);

// Clears the current thread's CLR error buffer inside the DLL.
ATLAS_NATIVE_API void atlas_clear_clr_error();
