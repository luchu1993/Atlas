#include "AtlasCore/client_entity.h"

#include <cstdint>

#include "AtlasCore/property_decoder.h"
#include "AtlasCore/span_reader.h"
#include "entitydef/entitydef_api.h"

namespace atlas {

namespace {

bool IsClientVisibleScope(uint8_t scope) {
  switch (scope) {
    case ATLAS_EDR_SCOPE_OWN_CLIENT:
    case ATLAS_EDR_SCOPE_OTHER_CLIENTS:
    case ATLAS_EDR_SCOPE_ALL_CLIENTS:
    case ATLAS_EDR_SCOPE_CELL_PUBLIC_AND_OWN:
    case ATLAS_EDR_SCOPE_BASE_AND_CLIENT:
      return true;
    default:
      return false;
  }
}

bool IsContainerType(uint8_t data_type) {
  return data_type == ATLAS_EDR_TYPE_LIST || data_type == ATLAS_EDR_TYPE_DICT;
}

// Flag-byte width follows the C# PropertiesEmitter table: ≤8 props → u8,
// ≤16 → u16, ≤32 → u32, else u64. Determined by the count of client-visible
// properties (scalars + containers), matching the server-side enum layout.
bool ReadFlagsByCount(SpanReader& r, int32_t visible_count, uint64_t& flags) {
  if (visible_count <= 8) {
    uint8_t v; if (!r.Read(v)) return false; flags = v; return true;
  }
  if (visible_count <= 16) {
    uint16_t v; if (!r.Read(v)) return false; flags = v; return true;
  }
  if (visible_count <= 32) {
    uint32_t v; if (!r.Read(v)) return false; flags = v; return true;
  }
  uint64_t v; if (!r.Read(v)) return false; flags = v; return true;
}

}  // namespace

void ClientEntity::BindDescriptor(const AtlasEdrEntity* descriptor, AtlasEdrContext* ctx) {
  descriptor_ = descriptor;
  edr_ctx_ = ctx;
  if (descriptor_ == nullptr) {
    properties_.clear();
    return;
  }
  const int32_t count = AtlasEdrEntityPropertyCount(descriptor_);
  // PropertyValue is move-only (unique_ptr alternatives) so assign(n, v)
  // is unavailable; resize() default-constructs each slot to monostate.
  properties_.clear();
  properties_.resize(count > 0 ? static_cast<size_t>(count) : 0u);
}

bool ClientEntity::ApplyDelta(SpanReader& reader) {
  if (descriptor_ == nullptr) return false;
  AtlasEdrContext* ctx = edr_ctx_;

  uint8_t section_mask;
  if (!reader.Read(section_mask)) return false;

  const int32_t prop_count = AtlasEdrEntityPropertyCount(descriptor_);
  int32_t visible_count = 0;
  for (int32_t i = 0; i < prop_count; ++i) {
    const AtlasEdrProperty* prop = AtlasEdrEntityPropertyAt(descriptor_, i);
    if (prop != nullptr && IsClientVisibleScope(AtlasEdrPropertyScope(prop))) ++visible_count;
  }

  if ((section_mask & 0x01) != 0) {
    uint64_t scalar_flags;
    if (!ReadFlagsByCount(reader, visible_count, scalar_flags)) return false;
    uint64_t bit = 0;
    for (int32_t i = 0; i < prop_count; ++i) {
      const AtlasEdrProperty* prop = AtlasEdrEntityPropertyAt(descriptor_, i);
      if (prop == nullptr) return false;
      if (!IsClientVisibleScope(AtlasEdrPropertyScope(prop))) continue;
      const uint8_t data_type = AtlasEdrPropertyDataType(prop);
      if (IsContainerType(data_type)) {
        ++bit;
        continue;
      }
      if ((scalar_flags & (uint64_t{1} << bit)) != 0) {
        const AtlasEdrDataTypeRef* ref = AtlasEdrPropertyTypeRef(prop);
        if (!DecodeValue(reader, data_type, ref, ctx, properties_[i])) return false;
        if (view_ != nullptr) view_->OnPropertyChanged(AtlasEdrPropertyIndex(prop));
      }
      ++bit;
    }
  }
  if ((section_mask & 0x02) != 0) {
    uint64_t container_flags;
    if (!ReadFlagsByCount(reader, visible_count, container_flags)) return false;
    uint64_t bit = 0;
    for (int32_t i = 0; i < prop_count; ++i) {
      const AtlasEdrProperty* prop = AtlasEdrEntityPropertyAt(descriptor_, i);
      if (prop == nullptr) return false;
      if (!IsClientVisibleScope(AtlasEdrPropertyScope(prop))) continue;
      const uint8_t data_type = AtlasEdrPropertyDataType(prop);
      if (!IsContainerType(data_type)) {
        ++bit;
        continue;
      }
      if ((container_flags & (uint64_t{1} << bit)) != 0) {
        const AtlasEdrDataTypeRef* ref = AtlasEdrPropertyTypeRef(prop);
        if (!DecodeContainerOps(reader, ref, ctx, properties_[i])) return false;
        if (view_ != nullptr) view_->OnPropertyChanged(AtlasEdrPropertyIndex(prop));
      }
      ++bit;
    }
  }
  if ((section_mask & 0x04) != 0) {
    // Component section per-slot dispatch lands when components ship to UE.
    return reader.Skip(reader.Remaining());
  }
  return true;
}

}  // namespace atlas
