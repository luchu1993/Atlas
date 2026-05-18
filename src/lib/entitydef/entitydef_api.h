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

// 32-byte SHA-256; NULL before Load. Buffer is ctx-owned, valid until Destroy.
ATLAS_EDR_API const uint8_t* AtlasEdrGetDigest(AtlasEdrContext* ctx);
ATLAS_EDR_API int32_t AtlasEdrGetDigestSize(AtlasEdrContext* ctx);

// Mirrors atlas::PropertyDataType.
typedef enum AtlasEdrDataType {
  ATLAS_EDR_TYPE_BOOL = 0,
  ATLAS_EDR_TYPE_INT8 = 1,
  ATLAS_EDR_TYPE_UINT8 = 2,
  ATLAS_EDR_TYPE_INT16 = 3,
  ATLAS_EDR_TYPE_UINT16 = 4,
  ATLAS_EDR_TYPE_INT32 = 5,
  ATLAS_EDR_TYPE_UINT32 = 6,
  ATLAS_EDR_TYPE_INT64 = 7,
  ATLAS_EDR_TYPE_UINT64 = 8,
  ATLAS_EDR_TYPE_FLOAT = 9,
  ATLAS_EDR_TYPE_DOUBLE = 10,
  ATLAS_EDR_TYPE_STRING = 11,
  ATLAS_EDR_TYPE_BYTES = 12,
  ATLAS_EDR_TYPE_VECTOR3 = 13,
  ATLAS_EDR_TYPE_QUATERNION = 14,
  ATLAS_EDR_TYPE_CUSTOM = 15,
  ATLAS_EDR_TYPE_LIST = 16,
  ATLAS_EDR_TYPE_DICT = 17,
  ATLAS_EDR_TYPE_STRUCT = 18,
  ATLAS_EDR_TYPE_INVALID = 0xFF,
} AtlasEdrDataType;

// Mirrors atlas::ReplicationScope.
typedef enum AtlasEdrScope {
  ATLAS_EDR_SCOPE_CELL_PRIVATE = 0,
  ATLAS_EDR_SCOPE_CELL_PUBLIC = 1,
  ATLAS_EDR_SCOPE_OWN_CLIENT = 2,
  ATLAS_EDR_SCOPE_OTHER_CLIENTS = 3,
  ATLAS_EDR_SCOPE_ALL_CLIENTS = 4,
  ATLAS_EDR_SCOPE_CELL_PUBLIC_AND_OWN = 5,
  ATLAS_EDR_SCOPE_BASE = 6,
  ATLAS_EDR_SCOPE_BASE_AND_CLIENT = 7,
} AtlasEdrScope;

typedef struct AtlasEdrEntity AtlasEdrEntity;
typedef struct AtlasEdrProperty AtlasEdrProperty;
typedef struct AtlasEdrDataTypeRef AtlasEdrDataTypeRef;
typedef struct AtlasEdrStruct AtlasEdrStruct;
typedef struct AtlasEdrStructField AtlasEdrStructField;
typedef struct AtlasEdrComponent AtlasEdrComponent;

// Pointers returned remain valid until AtlasEdrDestroy(ctx).
ATLAS_EDR_API const AtlasEdrEntity* AtlasEdrFindEntityById(AtlasEdrContext* ctx, uint16_t type_id);
ATLAS_EDR_API const AtlasEdrEntity* AtlasEdrFindEntityByName(AtlasEdrContext* ctx,
                                                              const char* name);

ATLAS_EDR_API uint16_t AtlasEdrEntityTypeId(const AtlasEdrEntity* entity);
ATLAS_EDR_API const char* AtlasEdrEntityName(const AtlasEdrEntity* entity);

ATLAS_EDR_API int32_t AtlasEdrEntityPropertyCount(const AtlasEdrEntity* entity);
ATLAS_EDR_API const AtlasEdrProperty* AtlasEdrEntityPropertyAt(const AtlasEdrEntity* entity,
                                                               int32_t index);

// 0 means missing — no valid rpc_id has a zero method index.
ATLAS_EDR_API uint32_t AtlasEdrFindRpcId(AtlasEdrContext* ctx, const char* entity_name,
                                          const char* method_name);

// Components share AtlasEdrProperty shape with entity bodies.
ATLAS_EDR_API const AtlasEdrComponent* AtlasEdrFindComponentById(AtlasEdrContext* ctx,
                                                                  uint16_t component_type_id);
ATLAS_EDR_API const AtlasEdrComponent* AtlasEdrFindComponentByName(AtlasEdrContext* ctx,
                                                                    const char* name);
ATLAS_EDR_API const char* AtlasEdrComponentName(const AtlasEdrComponent* comp);
ATLAS_EDR_API uint16_t AtlasEdrComponentTypeId(const AtlasEdrComponent* comp);
ATLAS_EDR_API int32_t AtlasEdrComponentPropertyCount(const AtlasEdrComponent* comp);
ATLAS_EDR_API const AtlasEdrProperty* AtlasEdrComponentPropertyAt(const AtlasEdrComponent* comp,
                                                                   int32_t index);

// Null when the slot is unallocated.
ATLAS_EDR_API const AtlasEdrComponent* AtlasEdrEntityComponentAtSlot(
    AtlasEdrContext* ctx, const AtlasEdrEntity* entity, uint8_t slot_idx);

ATLAS_EDR_API uint8_t AtlasEdrPropertyDataType(const AtlasEdrProperty* prop);
ATLAS_EDR_API uint8_t AtlasEdrPropertyScope(const AtlasEdrProperty* prop);
ATLAS_EDR_API uint16_t AtlasEdrPropertyIndex(const AtlasEdrProperty* prop);

// Returns null for pure scalars (their data_type alone pins the decode).
ATLAS_EDR_API const AtlasEdrDataTypeRef* AtlasEdrPropertyTypeRef(const AtlasEdrProperty* prop);

// Elem/Key return null for scalars + pure-struct refs; StructId only valid for
// kind == ATLAS_EDR_TYPE_STRUCT.
ATLAS_EDR_API uint8_t AtlasEdrDataTypeRefKind(const AtlasEdrDataTypeRef* ref);
ATLAS_EDR_API const AtlasEdrDataTypeRef* AtlasEdrDataTypeRefElem(const AtlasEdrDataTypeRef* ref);
ATLAS_EDR_API const AtlasEdrDataTypeRef* AtlasEdrDataTypeRefKey(const AtlasEdrDataTypeRef* ref);
ATLAS_EDR_API uint16_t AtlasEdrDataTypeRefStructId(const AtlasEdrDataTypeRef* ref);

ATLAS_EDR_API const AtlasEdrStruct* AtlasEdrFindStructById(AtlasEdrContext* ctx,
                                                            uint16_t struct_id);
ATLAS_EDR_API const AtlasEdrStruct* AtlasEdrFindStructByName(AtlasEdrContext* ctx,
                                                              const char* name);

ATLAS_EDR_API int32_t AtlasEdrStructFieldCount(const AtlasEdrStruct* s);
ATLAS_EDR_API const AtlasEdrStructField* AtlasEdrStructFieldAt(const AtlasEdrStruct* s,
                                                                int32_t index);

ATLAS_EDR_API uint8_t AtlasEdrStructFieldDataType(const AtlasEdrStructField* field);
ATLAS_EDR_API const AtlasEdrDataTypeRef* AtlasEdrStructFieldTypeRef(
    const AtlasEdrStructField* field);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // ATLAS_LIB_ENTITYDEF_ENTITYDEF_API_H_
