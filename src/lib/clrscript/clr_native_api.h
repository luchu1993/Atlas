#ifndef ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_H_
#define ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_H_

#include <cstdint>

#include "clrscript/clr_export.h"
#include "clrscript/clr_native_api_defs.h"

#define X(ret, name, params, call) ATLAS_NATIVE_API ret Atlas##name params;
ATLAS_NATIVE_API_TABLE(X)
#undef X

ATLAS_NATIVE_API uint32_t AtlasGetAbiVersion();

// provider must point to an atlas::INativeApiProvider and be set before CLR init.
ATLAS_NATIVE_API void AtlasSetNativeApiProvider(void* provider);

// Returns pointers to the DLL's TLS error functions.
ATLAS_NATIVE_API void* AtlasGetClrErrorSetFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorClearFn();
ATLAS_NATIVE_API void* AtlasGetClrErrorGetCodeFn();

ATLAS_NATIVE_API int32_t AtlasHasClrError();

// Copies the raw error message into buf and clears the buffer.
ATLAS_NATIVE_API int32_t AtlasReadClrError(char* buf, int32_t buf_len);

ATLAS_NATIVE_API void AtlasClearClrError();

#endif  // ATLAS_LIB_CLRSCRIPT_CLR_NATIVE_API_H_
