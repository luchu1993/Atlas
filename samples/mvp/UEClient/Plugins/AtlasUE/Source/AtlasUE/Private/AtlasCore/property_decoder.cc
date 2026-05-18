#include "AtlasCore/property_decoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AtlasCore/entity_view.h"
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

// Flag width mirrors PropertiesEmitter: ≤8 → u8, ≤16 → u16, ≤32 → u32, else u64.
// `visible_count` counts client-visible props (scalars + containers).
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

bool DecodeScalar(SpanReader& r, uint8_t data_type, PropertyValue& out) {
  switch (data_type) {
    case ATLAS_EDR_TYPE_BOOL: {
      uint8_t v; if (!r.Read(v)) return false; out = (v != 0); return true;
    }
    case ATLAS_EDR_TYPE_INT8: {
      int8_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_UINT8: {
      uint8_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_INT16: {
      int16_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_UINT16: {
      uint16_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_INT32: {
      int32_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_UINT32: {
      uint32_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_INT64: {
      int64_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_UINT64: {
      uint64_t v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_FLOAT: {
      float v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_DOUBLE: {
      double v; if (!r.Read(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_STRING: {
      std::string v; if (!r.ReadString(v)) return false; out = std::move(v); return true;
    }
    case ATLAS_EDR_TYPE_BYTES: {
      std::vector<uint8_t> v; if (!r.ReadBytes(v)) return false; out = std::move(v); return true;
    }
    case ATLAS_EDR_TYPE_VECTOR3: {
      Vec3 v; if (!r.ReadVec3(v)) return false; out = v; return true;
    }
    case ATLAS_EDR_TYPE_QUATERNION: {
      Quat v; if (!r.ReadQuat(v)) return false; out = v; return true;
    }
    default:
      return false;
  }
}

bool IsContainerKind(uint8_t kind) {
  return kind == ATLAS_EDR_TYPE_LIST || kind == ATLAS_EDR_TYPE_DICT;
}

bool DecodeStruct(SpanReader& r, const AtlasEdrStruct* desc, AtlasEdrContext* ctx,
                  PropertyValue& out) {
  if (desc == nullptr) return false;
  const int32_t field_count = AtlasEdrStructFieldCount(desc);
  auto value = std::make_unique<StructValue>();
  value->fields.resize(static_cast<size_t>(field_count));
  for (int32_t i = 0; i < field_count; ++i) {
    const AtlasEdrStructField* field = AtlasEdrStructFieldAt(desc, i);
    if (field == nullptr) return false;
    const uint8_t kind = AtlasEdrStructFieldDataType(field);
    const AtlasEdrDataTypeRef* ref = AtlasEdrStructFieldTypeRef(field);
    if (!DecodeValue(r, kind, ref, ctx, value->fields[static_cast<size_t>(i)])) {
      return false;
    }
  }
  out = std::move(value);
  return true;
}

bool DecodeIntegralList(SpanReader& r, const AtlasEdrDataTypeRef* ref, AtlasEdrContext* ctx,
                       PropertyValue& out) {
  uint16_t count;
  if (!r.Read(count)) return false;
  const AtlasEdrDataTypeRef* elem_ref = AtlasEdrDataTypeRefElem(ref);
  if (elem_ref == nullptr) return false;
  const uint8_t elem_kind = AtlasEdrDataTypeRefKind(elem_ref);
  auto value = std::make_unique<ListValue>();
  value->items.resize(count);
  for (uint16_t i = 0; i < count; ++i) {
    if (!DecodeValue(r, elem_kind, elem_ref, ctx, value->items[i])) return false;
  }
  out = std::move(value);
  return true;
}

bool DecodeIntegralDict(SpanReader& r, const AtlasEdrDataTypeRef* ref, AtlasEdrContext* ctx,
                       PropertyValue& out) {
  uint16_t count;
  if (!r.Read(count)) return false;
  const AtlasEdrDataTypeRef* key_ref = AtlasEdrDataTypeRefKey(ref);
  const AtlasEdrDataTypeRef* elem_ref = AtlasEdrDataTypeRefElem(ref);
  if (key_ref == nullptr || elem_ref == nullptr) return false;
  const uint8_t key_kind = AtlasEdrDataTypeRefKind(key_ref);
  const uint8_t elem_kind = AtlasEdrDataTypeRefKind(elem_ref);
  auto value = std::make_unique<DictValue>();
  value->entries.resize(count);
  for (uint16_t i = 0; i < count; ++i) {
    if (!DecodeValue(r, key_kind, key_ref, ctx, value->entries[i].first)) return false;
    if (!DecodeValue(r, elem_kind, elem_ref, ctx, value->entries[i].second)) return false;
  }
  out = std::move(value);
  return true;
}

ListValue* AsList(PropertyValue& v) {
  auto* slot = std::get_if<std::unique_ptr<ListValue>>(&v);
  return slot != nullptr ? slot->get() : nullptr;
}

DictValue* AsDict(PropertyValue& v) {
  auto* slot = std::get_if<std::unique_ptr<DictValue>>(&v);
  return slot != nullptr ? slot->get() : nullptr;
}

StructValue* AsStruct(PropertyValue& v) {
  auto* slot = std::get_if<std::unique_ptr<StructValue>>(&v);
  return slot != nullptr ? slot->get() : nullptr;
}

std::pair<PropertyValue, PropertyValue>* FindDictEntry(DictValue& d, const PropertyValue& key) {
  for (auto& entry : d.entries) {
    if (entry.first == key) return &entry;
  }
  return nullptr;
}

bool DecodeListOps(SpanReader& r, const AtlasEdrDataTypeRef* ref, AtlasEdrContext* ctx,
                   ListValue& list) {
  const AtlasEdrDataTypeRef* elem_ref = AtlasEdrDataTypeRefElem(ref);
  if (elem_ref == nullptr) return false;
  const uint8_t elem_kind = AtlasEdrDataTypeRefKind(elem_ref);

  uint16_t op_count;
  if (!r.Read(op_count)) return false;
  for (uint16_t i = 0; i < op_count; ++i) {
    uint8_t raw_kind;
    if (!r.Read(raw_kind)) return false;
    const OpKind kind = static_cast<OpKind>(raw_kind);
    switch (kind) {
      case OpKind::kSet: {
        uint16_t slot;
        if (!r.Read(slot)) return false;
        if (slot >= list.items.size()) return false;
        if (!DecodeValue(r, elem_kind, elem_ref, ctx, list.items[slot])) return false;
        break;
      }
      case OpKind::kListSplice: {
        uint16_t start;
        uint16_t end;
        uint16_t vcount;
        if (!r.Read(start) || !r.Read(end) || !r.Read(vcount)) return false;
        if (start > end || end > list.items.size()) return false;
        list.items.erase(list.items.begin() + start, list.items.begin() + end);
        for (uint16_t j = 0; j < vcount; ++j) {
          PropertyValue tmp;
          if (!DecodeValue(r, elem_kind, elem_ref, ctx, tmp)) return false;
          list.items.insert(list.items.begin() + start + j, std::move(tmp));
        }
        break;
      }
      case OpKind::kClear: {
        list.items.clear();
        break;
      }
      case OpKind::kStructFieldSet: {
        if (elem_kind != ATLAS_EDR_TYPE_STRUCT) return false;
        uint16_t slot;
        uint8_t field_id;
        if (!r.Read(slot) || !r.Read(field_id)) return false;
        if (slot >= list.items.size()) return false;
        StructValue* sv = AsStruct(list.items[slot]);
        if (sv == nullptr) return false;
        const AtlasEdrStruct* struct_desc =
            AtlasEdrFindStructById(ctx, AtlasEdrDataTypeRefStructId(elem_ref));
        if (struct_desc == nullptr) return false;
        if (field_id >= AtlasEdrStructFieldCount(struct_desc)) return false;
        const AtlasEdrStructField* field = AtlasEdrStructFieldAt(struct_desc, field_id);
        if (field == nullptr) return false;
        const uint8_t fk = AtlasEdrStructFieldDataType(field);
        const AtlasEdrDataTypeRef* fr = AtlasEdrStructFieldTypeRef(field);
        if (!DecodeValue(r, fk, fr, ctx, sv->fields[field_id])) return false;
        break;
      }
      default:
        return false;
    }
  }

  // Nested-container child-dirty section. Each dirty slot carries a fresh
  // recursive op log; non-dirty slots stay as set by the ops above.
  if (IsContainerKind(elem_kind)) {
    uint32_t dirty_count;
    if (!r.ReadPackedUInt32(dirty_count)) return false;
    for (uint32_t d = 0; d < dirty_count; ++d) {
      uint16_t slot;
      if (!r.Read(slot)) return false;
      if (slot >= list.items.size()) return false;
      if (!DecodeContainerOps(r, elem_ref, ctx, list.items[slot])) return false;
    }
  }
  return true;
}

bool DecodeDictOps(SpanReader& r, const AtlasEdrDataTypeRef* ref, AtlasEdrContext* ctx,
                   DictValue& dict) {
  const AtlasEdrDataTypeRef* key_ref = AtlasEdrDataTypeRefKey(ref);
  const AtlasEdrDataTypeRef* elem_ref = AtlasEdrDataTypeRefElem(ref);
  if (key_ref == nullptr || elem_ref == nullptr) return false;
  const uint8_t key_kind = AtlasEdrDataTypeRefKind(key_ref);
  const uint8_t elem_kind = AtlasEdrDataTypeRefKind(elem_ref);

  uint16_t op_count;
  if (!r.Read(op_count)) return false;
  for (uint16_t i = 0; i < op_count; ++i) {
    uint8_t raw_kind;
    if (!r.Read(raw_kind)) return false;
    const OpKind kind = static_cast<OpKind>(raw_kind);
    switch (kind) {
      case OpKind::kDictSet: {
        PropertyValue key;
        PropertyValue val;
        if (!DecodeValue(r, key_kind, key_ref, ctx, key)) return false;
        if (!DecodeValue(r, elem_kind, elem_ref, ctx, val)) return false;
        if (auto* found = FindDictEntry(dict, key)) {
          found->second = std::move(val);
        } else {
          dict.entries.emplace_back(std::move(key), std::move(val));
        }
        break;
      }
      case OpKind::kDictErase: {
        PropertyValue key;
        if (!DecodeValue(r, key_kind, key_ref, ctx, key)) return false;
        for (auto it = dict.entries.begin(); it != dict.entries.end(); ++it) {
          if (it->first == key) {
            dict.entries.erase(it);
            break;
          }
        }
        break;
      }
      case OpKind::kClear: {
        dict.entries.clear();
        break;
      }
      default:
        return false;
    }
  }

  if (IsContainerKind(elem_kind)) {
    uint32_t dirty_count;
    if (!r.ReadPackedUInt32(dirty_count)) return false;
    for (uint32_t d = 0; d < dirty_count; ++d) {
      PropertyValue key;
      if (!DecodeValue(r, key_kind, key_ref, ctx, key)) return false;
      auto* found = FindDictEntry(dict, key);
      if (found == nullptr) return false;
      if (!DecodeContainerOps(r, elem_ref, ctx, found->second)) return false;
    }
  }
  return true;
}

}  // namespace

bool DecodeValue(SpanReader& reader, uint8_t data_type, const AtlasEdrDataTypeRef* ref,
                 AtlasEdrContext* ctx, PropertyValue& out) {
  if (data_type == ATLAS_EDR_TYPE_STRUCT) {
    if (ref == nullptr) return false;
    return DecodeStruct(reader, AtlasEdrFindStructById(ctx, AtlasEdrDataTypeRefStructId(ref)), ctx,
                        out);
  }
  if (data_type == ATLAS_EDR_TYPE_LIST) {
    if (ref == nullptr) return false;
    return DecodeIntegralList(reader, ref, ctx, out);
  }
  if (data_type == ATLAS_EDR_TYPE_DICT) {
    if (ref == nullptr) return false;
    return DecodeIntegralDict(reader, ref, ctx, out);
  }
  return DecodeScalar(reader, data_type, out);
}

bool DecodeContainerOps(SpanReader& reader, const AtlasEdrDataTypeRef* ref, AtlasEdrContext* ctx,
                        PropertyValue& out) {
  if (ref == nullptr) return false;
  const uint8_t kind = AtlasEdrDataTypeRefKind(ref);
  if (kind == ATLAS_EDR_TYPE_LIST) {
    if (AsList(out) == nullptr) out = std::make_unique<ListValue>();
    return DecodeListOps(reader, ref, ctx, *AsList(out));
  }
  if (kind == ATLAS_EDR_TYPE_DICT) {
    if (AsDict(out) == nullptr) out = std::make_unique<DictValue>();
    return DecodeDictOps(reader, ref, ctx, *AsDict(out));
  }
  return false;
}

bool ApplyPropertyDelta(SpanReader& reader, AtlasEdrContext* ctx, int32_t props_count,
                        const void* desc, PropertyAtFn props_at,
                        std::vector<PropertyValue>& slots, EntityView* view,
                        uint8_t* section_mask_out) {
  uint8_t section_mask;
  if (!reader.Read(section_mask)) return false;
  if (section_mask_out != nullptr) *section_mask_out = section_mask;

  int32_t visible_count = 0;
  for (int32_t i = 0; i < props_count; ++i) {
    const AtlasEdrProperty* prop = props_at(desc, i);
    if (prop != nullptr && IsClientVisibleScope(AtlasEdrPropertyScope(prop))) ++visible_count;
  }

  if ((section_mask & 0x01) != 0) {
    uint64_t scalar_flags;
    if (!ReadFlagsByCount(reader, visible_count, scalar_flags)) return false;
    uint64_t bit = 0;
    for (int32_t i = 0; i < props_count; ++i) {
      const AtlasEdrProperty* prop = props_at(desc, i);
      if (prop == nullptr) return false;
      if (!IsClientVisibleScope(AtlasEdrPropertyScope(prop))) continue;
      const uint8_t data_type = AtlasEdrPropertyDataType(prop);
      if (IsContainerType(data_type)) {
        ++bit;
        continue;
      }
      if ((scalar_flags & (uint64_t{1} << bit)) != 0) {
        const AtlasEdrDataTypeRef* ref = AtlasEdrPropertyTypeRef(prop);
        if (!DecodeValue(reader, data_type, ref, ctx, slots[i])) return false;
        if (view != nullptr) view->OnPropertyChanged(AtlasEdrPropertyIndex(prop));
      }
      ++bit;
    }
  }
  if ((section_mask & 0x02) != 0) {
    uint64_t container_flags;
    if (!ReadFlagsByCount(reader, visible_count, container_flags)) return false;
    uint64_t bit = 0;
    for (int32_t i = 0; i < props_count; ++i) {
      const AtlasEdrProperty* prop = props_at(desc, i);
      if (prop == nullptr) return false;
      if (!IsClientVisibleScope(AtlasEdrPropertyScope(prop))) continue;
      const uint8_t data_type = AtlasEdrPropertyDataType(prop);
      if (!IsContainerType(data_type)) {
        ++bit;
        continue;
      }
      if ((container_flags & (uint64_t{1} << bit)) != 0) {
        const AtlasEdrDataTypeRef* ref = AtlasEdrPropertyTypeRef(prop);
        if (!DecodeContainerOps(reader, ref, ctx, slots[i])) return false;
        if (view != nullptr) view->OnPropertyChanged(AtlasEdrPropertyIndex(prop));
      }
      ++bit;
    }
  }
  // Component section (bit 0x04) is entity-only; caller dispatches after
  // this returns true. Components themselves don't carry sub-components.
  return true;
}

}  // namespace atlas
