#ifndef ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_H_

#include <cstddef>
#include <cstdint>

#include "clrscript/clr_export.h"
#include "clrscript/clr_native_api_defs.h"

// ============================================================================
// Atlas* C-linkage export function declarations
// ============================================================================
//
// Generated from ATLAS_NATIVE_API_TABLE in clr_native_api_defs.hpp.
// To add a new export, edit clr_native_api_defs.hpp ONLY — this file and
// clr_native_api.cpp update automatically.
//
// AtlasGetAbiVersion() is declared separately (not in the table) because it
// does not delegate to INativeApiProvider.

#define X(ret, name, params, call) ATLAS_NATIVE_API ret Atlas##name params;
ATLAS_NATIVE_API_TABLE(X)
#undef X

// ---- ABI version ------------------------------------------------------------
// C# startup code calls this to verify the native/managed struct layouts match.
// Returns atlas::kAtlasAbiVersion.

ATLAS_NATIVE_API uint32_t AtlasGetAbiVersion();

// ---- Provider registration --------------------------------------------------
// Register the INativeApiProvider for this process so that Atlas* functions
// can delegate to the correct implementation.  Must be called once before
// ClrHost::Initialize().
//
// provider must point to an atlas::INativeApiProvider instance.
// Exported so that the calling process can set the provider even when it
// has loaded atlas_engine as a shared library (avoiding the duplicate-global
// problem that would arise from separate static linkage of atlas_clrscript).

ATLAS_NATIVE_API void AtlasSetNativeApiProvider(void* provider);

// ---- Error bridge query / write (DLL-side) ----------------------------------
// These functions read/write the TLS error buffer INSIDE atlas_engine.dll.
// Processes that also statically link atlas_clrscript have a SEPARATE copy of
// the TLS buffer.  Use these exports to ensure both C# (via clr_error_set_fn)
// and C++ (via AtlasHasClrError / AtlasReadClrError / AtlasClearClrError)
// access the SAME buffer inside the DLL.
//
// Intended use (e.g. in test drivers):
//   1. Obtain the DLL's clr_error_set address via AtlasGetClrErrorSetFn().
//   2. Pass it in ClrBootstrapArgs::error_set so C# writes into the DLL buffer.
//   3. Query the DLL buffer via AtlasHasClrError / AtlasReadClrError.

// Returns function pointers to the DLL's TLS error functions.
// Cast each result to the indicated function-pointer type before calling.
//   AtlasGetClrErrorSetFn   → void(*)(int32_t, const char*, int32_t)
//   AtlasGetClrErrorClearFn → void(*)()
//   AtlasGetClrErrorGetCodeFn → int32_t(*)()
ATLAS_NATIVE_API void* AtlasGetClrErrorSetFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorClearFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorGetCodeFn();

// Returns 1 if the current thread has an unread CLR error, 0 otherwise.
ATLAS_NATIVE_API int32_t AtlasHasClrError();

// Copies the raw error message into buf (up to buf_len bytes, not null-terminated).
// Returns the error HResult.  Clears the buffer.
// If buf is null or buf_len <= 0, just clears and returns the code.
ATLAS_NATIVE_API int32_t AtlasReadClrError(char* buf, int32_t buf_len);

// Clears the current thread's CLR error buffer inside the DLL.
ATLAS_NATIVE_API void AtlasClearClrError();

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_H_
