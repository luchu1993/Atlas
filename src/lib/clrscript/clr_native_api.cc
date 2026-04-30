#include "clrscript/clr_native_api.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "clrscript/clr_error.h"
#include "clrscript/native_api_provider.h"

#define X(ret, name, params, call)          \
  ATLAS_NATIVE_API ret Atlas##name params { \
    call;                                   \
  }
ATLAS_NATIVE_API_TABLE(X)
#undef X

ATLAS_NATIVE_API uint32_t AtlasGetAbiVersion() {
  return atlas::kAtlasAbiVersion;
}

ATLAS_NATIVE_API void AtlasSetNativeApiProvider(void* provider) {
  atlas::SetNativeApiProvider(static_cast<atlas::INativeApiProvider*>(provider));
}

ATLAS_NATIVE_API void* AtlasGetClrErrorSetFn() {
  return reinterpret_cast<void*>(&atlas::ClrErrorSet);
}

ATLAS_NATIVE_API void* AtlasGetClrErrorClearFn() {
  return reinterpret_cast<void*>(&atlas::ClrErrorClear);
}

ATLAS_NATIVE_API void* AtlasGetClrErrorGetCodeFn() {
  return reinterpret_cast<void*>(&atlas::ClrErrorGetCode);
}

ATLAS_NATIVE_API int32_t AtlasHasClrError() {
  return atlas::HasClrError() ? 1 : 0;
}

ATLAS_NATIVE_API int32_t AtlasReadClrError(char* buf, int32_t buf_len) {
  const int32_t kCode = atlas::t_clr_error.error_code;
  const int32_t kLen = atlas::t_clr_error.message_length;

  if (buf != nullptr && buf_len > 0 && kLen > 0) {
    const int32_t kCopyLen = std::min(kLen, buf_len);
    std::memcpy(buf, atlas::t_clr_error.message, static_cast<std::size_t>(kCopyLen));
  }

  atlas::ClearClrError();
  return kCode;
}

ATLAS_NATIVE_API void AtlasClearClrError() {
  atlas::ClearClrError();
}
