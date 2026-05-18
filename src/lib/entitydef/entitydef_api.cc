#include "entitydef/entitydef_api.h"

#include <cstddef>
#include <filesystem>
#include <new>
#include <span>
#include <string>

#include "entitydef/entity_def_registry.h"

namespace {

thread_local std::string g_global_last_error;

auto AbiCompatible(uint32_t expected) -> bool {
  constexpr uint32_t kOur = ATLAS_EDR_ABI_VERSION;
  const uint32_t our_major = (kOur >> 24) & 0xFFu;
  const uint32_t our_minor = (kOur >> 16) & 0xFFu;
  const uint32_t exp_major = (expected >> 24) & 0xFFu;
  const uint32_t exp_minor = (expected >> 16) & 0xFFu;
  return exp_major == our_major && exp_minor <= our_minor;
}

}  // namespace

struct AtlasEdrContext {
  atlas::EntityDefRegistry registry;
  std::string last_error;
  bool loaded{false};
};

extern "C" {

uint32_t AtlasEdrGetAbiVersion(void) { return ATLAS_EDR_ABI_VERSION; }

const char* AtlasEdrLastError(AtlasEdrContext* ctx) {
  if (!ctx) return AtlasEdrGlobalLastError();
  return ctx->last_error.empty() ? "" : ctx->last_error.c_str();
}

const char* AtlasEdrGlobalLastError(void) {
  return g_global_last_error.empty() ? "" : g_global_last_error.c_str();
}

AtlasEdrContext* AtlasEdrCreate(uint32_t expected_abi) {
  if (!AbiCompatible(expected_abi)) {
    g_global_last_error = "ABI mismatch";
    return nullptr;
  }
  try {
    return new AtlasEdrContext{};
  } catch (const std::bad_alloc&) {
    g_global_last_error = "out of memory";
    return nullptr;
  }
}

void AtlasEdrDestroy(AtlasEdrContext* ctx) { delete ctx; }

int32_t AtlasEdrLoadFromFile(AtlasEdrContext* ctx, const char* path) {
  if (!ctx || !path) return ATLAS_EDR_ERR_INVAL;
  auto result = ctx->registry.RegisterFromBinaryFile(std::filesystem::path(path));
  if (!result.HasValue()) {
    ctx->last_error = std::string(result.Error().Message());
    return ATLAS_EDR_ERR_PARSE;
  }
  ctx->loaded = true;
  return ATLAS_EDR_OK;
}

int32_t AtlasEdrLoadFromBuffer(AtlasEdrContext* ctx, const uint8_t* data, int32_t len) {
  if (!ctx || !data || len <= 0) return ATLAS_EDR_ERR_INVAL;
  auto buf = std::span<const std::byte>(
      reinterpret_cast<const std::byte*>(data), static_cast<size_t>(len));
  auto result = ctx->registry.RegisterFromBinaryBuffer(buf);
  if (!result.HasValue()) {
    ctx->last_error = std::string(result.Error().Message());
    return ATLAS_EDR_ERR_PARSE;
  }
  ctx->loaded = true;
  return ATLAS_EDR_OK;
}

const uint8_t* AtlasEdrGetDigest(AtlasEdrContext* ctx) {
  if (!ctx || !ctx->loaded) return nullptr;
  return ctx->registry.Digest().data();
}

int32_t AtlasEdrGetDigestSize(AtlasEdrContext* ctx) {
  if (!ctx || !ctx->loaded) return 0;
  return static_cast<int32_t>(ctx->registry.Digest().size());
}

const AtlasEdrEntity* AtlasEdrFindEntityById(AtlasEdrContext* ctx, uint16_t type_id) {
  if (!ctx) return nullptr;
  return reinterpret_cast<const AtlasEdrEntity*>(ctx->registry.FindById(type_id));
}

const AtlasEdrEntity* AtlasEdrFindEntityByName(AtlasEdrContext* ctx, const char* name) {
  if (!ctx || !name) return nullptr;
  return reinterpret_cast<const AtlasEdrEntity*>(ctx->registry.FindByName(name));
}

uint16_t AtlasEdrEntityTypeId(const AtlasEdrEntity* entity) {
  if (!entity) return 0;
  return reinterpret_cast<const atlas::EntityTypeDescriptor*>(entity)->type_id;
}

const char* AtlasEdrEntityName(const AtlasEdrEntity* entity) {
  if (!entity) return "";
  return reinterpret_cast<const atlas::EntityTypeDescriptor*>(entity)->name.c_str();
}

uint32_t AtlasEdrFindRpcId(AtlasEdrContext* ctx, const char* entity_name,
                            const char* method_name) {
  if (!ctx || !entity_name || !method_name) return 0;
  const auto* desc = ctx->registry.FindByName(entity_name);
  if (desc == nullptr) return 0;
  for (const auto& rpc : desc->rpcs) {
    if (rpc.name == method_name) return rpc.rpc_id;
  }
  return 0;
}

const AtlasEdrComponent* AtlasEdrFindComponentById(AtlasEdrContext* ctx,
                                                    uint16_t component_type_id) {
  if (!ctx) return nullptr;
  return reinterpret_cast<const AtlasEdrComponent*>(
      ctx->registry.FindComponentById(component_type_id));
}

const AtlasEdrComponent* AtlasEdrFindComponentByName(AtlasEdrContext* ctx, const char* name) {
  if (!ctx || !name) return nullptr;
  return reinterpret_cast<const AtlasEdrComponent*>(ctx->registry.FindComponentByName(name));
}

const char* AtlasEdrComponentName(const AtlasEdrComponent* comp) {
  if (!comp) return "";
  return reinterpret_cast<const atlas::ComponentDescriptor*>(comp)->name.c_str();
}

uint16_t AtlasEdrComponentTypeId(const AtlasEdrComponent* comp) {
  if (!comp) return 0;
  return reinterpret_cast<const atlas::ComponentDescriptor*>(comp)->component_type_id;
}

int32_t AtlasEdrComponentPropertyCount(const AtlasEdrComponent* comp) {
  if (!comp) return 0;
  return static_cast<int32_t>(
      reinterpret_cast<const atlas::ComponentDescriptor*>(comp)->properties.size());
}

const AtlasEdrProperty* AtlasEdrComponentPropertyAt(const AtlasEdrComponent* comp, int32_t index) {
  if (!comp || index < 0) return nullptr;
  const auto* desc = reinterpret_cast<const atlas::ComponentDescriptor*>(comp);
  if (static_cast<size_t>(index) >= desc->properties.size()) return nullptr;
  return reinterpret_cast<const AtlasEdrProperty*>(&desc->properties[index]);
}

const AtlasEdrComponent* AtlasEdrEntityComponentAtSlot(AtlasEdrContext* ctx,
                                                        const AtlasEdrEntity* entity,
                                                        uint8_t slot_idx) {
  if (!ctx || !entity) return nullptr;
  const auto* desc = reinterpret_cast<const atlas::EntityTypeDescriptor*>(entity);
  for (const auto& slot : desc->slots) {
    if (slot.slot_idx == slot_idx) {
      return reinterpret_cast<const AtlasEdrComponent*>(
          ctx->registry.FindComponentById(slot.component_type_id));
    }
  }
  return nullptr;
}

int32_t AtlasEdrEntityPropertyCount(const AtlasEdrEntity* entity) {
  if (!entity) return 0;
  const auto* desc = reinterpret_cast<const atlas::EntityTypeDescriptor*>(entity);
  return static_cast<int32_t>(desc->properties.size());
}

const AtlasEdrProperty* AtlasEdrEntityPropertyAt(const AtlasEdrEntity* entity, int32_t index) {
  if (!entity || index < 0) return nullptr;
  const auto* desc = reinterpret_cast<const atlas::EntityTypeDescriptor*>(entity);
  if (static_cast<size_t>(index) >= desc->properties.size()) return nullptr;
  return reinterpret_cast<const AtlasEdrProperty*>(&desc->properties[index]);
}

uint8_t AtlasEdrPropertyDataType(const AtlasEdrProperty* prop) {
  if (!prop) return ATLAS_EDR_TYPE_INVALID;
  const auto* p = reinterpret_cast<const atlas::PropertyDescriptor*>(prop);
  return static_cast<uint8_t>(p->data_type);
}

uint8_t AtlasEdrPropertyScope(const AtlasEdrProperty* prop) {
  if (!prop) return 0;
  const auto* p = reinterpret_cast<const atlas::PropertyDescriptor*>(prop);
  return static_cast<uint8_t>(p->scope);
}

uint16_t AtlasEdrPropertyIndex(const AtlasEdrProperty* prop) {
  if (!prop) return 0;
  const auto* p = reinterpret_cast<const atlas::PropertyDescriptor*>(prop);
  return p->index;
}

const AtlasEdrDataTypeRef* AtlasEdrPropertyTypeRef(const AtlasEdrProperty* prop) {
  if (!prop) return nullptr;
  const auto* p = reinterpret_cast<const atlas::PropertyDescriptor*>(prop);
  if (!p->type_ref.has_value()) return nullptr;
  return reinterpret_cast<const AtlasEdrDataTypeRef*>(&*p->type_ref);
}

uint8_t AtlasEdrDataTypeRefKind(const AtlasEdrDataTypeRef* ref) {
  if (!ref) return ATLAS_EDR_TYPE_INVALID;
  const auto* r = reinterpret_cast<const atlas::DataTypeRef*>(ref);
  return static_cast<uint8_t>(r->kind);
}

const AtlasEdrDataTypeRef* AtlasEdrDataTypeRefElem(const AtlasEdrDataTypeRef* ref) {
  if (!ref) return nullptr;
  const auto* r = reinterpret_cast<const atlas::DataTypeRef*>(ref);
  return reinterpret_cast<const AtlasEdrDataTypeRef*>(r->elem.get());
}

const AtlasEdrDataTypeRef* AtlasEdrDataTypeRefKey(const AtlasEdrDataTypeRef* ref) {
  if (!ref) return nullptr;
  const auto* r = reinterpret_cast<const atlas::DataTypeRef*>(ref);
  return reinterpret_cast<const AtlasEdrDataTypeRef*>(r->key.get());
}

uint16_t AtlasEdrDataTypeRefStructId(const AtlasEdrDataTypeRef* ref) {
  if (!ref) return 0;
  const auto* r = reinterpret_cast<const atlas::DataTypeRef*>(ref);
  return r->struct_id;
}

const AtlasEdrStruct* AtlasEdrFindStructById(AtlasEdrContext* ctx, uint16_t struct_id) {
  if (!ctx) return nullptr;
  return reinterpret_cast<const AtlasEdrStruct*>(ctx->registry.FindStructById(struct_id));
}

const AtlasEdrStruct* AtlasEdrFindStructByName(AtlasEdrContext* ctx, const char* name) {
  if (!ctx || !name) return nullptr;
  return reinterpret_cast<const AtlasEdrStruct*>(ctx->registry.FindStructByName(name));
}

int32_t AtlasEdrStructFieldCount(const AtlasEdrStruct* s) {
  if (!s) return 0;
  const auto* desc = reinterpret_cast<const atlas::StructDescriptor*>(s);
  return static_cast<int32_t>(desc->fields.size());
}

const AtlasEdrStructField* AtlasEdrStructFieldAt(const AtlasEdrStruct* s, int32_t index) {
  if (!s || index < 0) return nullptr;
  const auto* desc = reinterpret_cast<const atlas::StructDescriptor*>(s);
  if (static_cast<size_t>(index) >= desc->fields.size()) return nullptr;
  return reinterpret_cast<const AtlasEdrStructField*>(&desc->fields[index]);
}

uint8_t AtlasEdrStructFieldDataType(const AtlasEdrStructField* field) {
  if (!field) return ATLAS_EDR_TYPE_INVALID;
  const auto* f = reinterpret_cast<const atlas::FieldDescriptor*>(field);
  return static_cast<uint8_t>(f->type.kind);
}

const AtlasEdrDataTypeRef* AtlasEdrStructFieldTypeRef(const AtlasEdrStructField* field) {
  if (!field) return nullptr;
  const auto* f = reinterpret_cast<const atlas::FieldDescriptor*>(field);
  return reinterpret_cast<const AtlasEdrDataTypeRef*>(&f->type);
}

}  // extern "C"
