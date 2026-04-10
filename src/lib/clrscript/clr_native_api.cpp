#include "clrscript/clr_native_api.hpp"

#include "clrscript/native_api_provider.hpp"

#include <cstddef>

// ============================================================================
// atlas_* export function implementations — generated from the API table
// ============================================================================
//
// Each function delegates to the INativeApiProvider registered at startup.
// To add a new export, edit clr_native_api_defs.hpp ONLY.

#define X(ret, name, params, call)           \
    ATLAS_NATIVE_API ret atlas_##name params \
    {                                        \
        call;                                \
    }
ATLAS_NATIVE_API_TABLE(X)
#undef X

// ---- ABI version ------------------------------------------------------------

ATLAS_NATIVE_API uint32_t atlas_get_abi_version()
{
    return atlas::kAtlasAbiVersion;
}
