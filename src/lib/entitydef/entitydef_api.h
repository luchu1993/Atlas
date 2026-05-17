#ifndef ATLAS_LIB_ENTITYDEF_ENTITYDEF_API_H_
#define ATLAS_LIB_ENTITYDEF_ENTITYDEF_API_H_

#include <stdint.h>

#include "entitydef/entitydef_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// Layout: [MAJOR:8][MINOR:8][PATCH:16]. Create rejects MAJOR mismatch or
// caller MINOR > our MINOR.
#define ATLAS_EDR_ABI_VERSION 0x01000000u

#define ATLAS_EDR_OK 0
#define ATLAS_EDR_ERR_INVAL -22
#define ATLAS_EDR_ERR_NOMEM -12
#define ATLAS_EDR_ERR_IO -5
#define ATLAS_EDR_ERR_PARSE -28
#define ATLAS_EDR_ERR_ABI -1000

typedef struct AtlasEdrContext AtlasEdrContext;

ATLAS_EDR_API uint32_t AtlasEdrGetAbiVersion(void);

// View valid until the next API call on the same ctx.
ATLAS_EDR_API const char* AtlasEdrLastError(AtlasEdrContext* ctx);

// Thread-local; only meaningful immediately after AtlasEdrCreate failure.
ATLAS_EDR_API const char* AtlasEdrGlobalLastError(void);

// Returns NULL on ABI mismatch / OOM.
ATLAS_EDR_API AtlasEdrContext* AtlasEdrCreate(uint32_t expected_abi);

ATLAS_EDR_API void AtlasEdrDestroy(AtlasEdrContext* ctx);

ATLAS_EDR_API int32_t AtlasEdrLoadFromFile(AtlasEdrContext* ctx, const char* path);
ATLAS_EDR_API int32_t AtlasEdrLoadFromBuffer(AtlasEdrContext* ctx, const uint8_t* data, int32_t len);

// 32-byte SHA-256 of the entity_defs surface; valid only after a successful
// Load. Returns NULL before Load; buffer is owned by ctx and valid until
// Destroy.
ATLAS_EDR_API const uint8_t* AtlasEdrGetDigest(AtlasEdrContext* ctx);
ATLAS_EDR_API int32_t AtlasEdrGetDigestSize(AtlasEdrContext* ctx);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ATLAS_LIB_ENTITYDEF_ENTITYDEF_API_H_
