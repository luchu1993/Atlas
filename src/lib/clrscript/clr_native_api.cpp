#include "clrscript/clr_native_api.hpp"

#include "clrscript/clr_error.hpp"
#include "clrscript/native_api_provider.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

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

// ---- Provider registration --------------------------------------------------

ATLAS_NATIVE_API void atlas_set_native_api_provider(void* provider)
{
    atlas::set_native_api_provider(static_cast<atlas::INativeApiProvider*>(provider));
}

// ---- Error bridge (DLL-side TLS) --------------------------------------------
//
// These functions expose the DLL's own copy of t_clr_error and clr_error_set.
// Test drivers that separately link atlas_clrscript (creating a second TLS copy)
// must use these exports so that C# (writing) and C++ (reading) both access the
// SAME TLS buffer inside this DLL.

ATLAS_NATIVE_API void* atlas_get_clr_error_set_fn()
{
    return reinterpret_cast<void*>(&atlas::clr_error_set);
}

ATLAS_NATIVE_API void* atlas_get_clr_error_clear_fn()
{
    return reinterpret_cast<void*>(&atlas::clr_error_clear);
}

ATLAS_NATIVE_API void* atlas_get_clr_error_get_code_fn()
{
    return reinterpret_cast<void*>(&atlas::clr_error_get_code);
}

ATLAS_NATIVE_API int32_t atlas_has_clr_error()
{
    return atlas::has_clr_error() ? 1 : 0;
}

ATLAS_NATIVE_API int32_t atlas_read_clr_error(char* buf, int32_t buf_len)
{
    const int32_t code = atlas::t_clr_error.error_code;
    const int32_t len = atlas::t_clr_error.message_length;

    if (buf != nullptr && buf_len > 0 && len > 0)
    {
        const int32_t copy_len = std::min(len, buf_len);
        std::memcpy(buf, atlas::t_clr_error.message, static_cast<std::size_t>(copy_len));
    }

    atlas::clear_clr_error();
    return code;
}

ATLAS_NATIVE_API void atlas_clear_clr_error()
{
    atlas::clear_clr_error();
}
