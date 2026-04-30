#include "entitydef/entity_def_registry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>

#include "foundation/log.h"
#include "serialization/binary_stream.h"

namespace atlas {

// ReadDataTypeRef reads [kind byte][body]; ReadDataTypeRefBody reads only
// the body with kind supplied by the caller. The split exists because the
// top-level kind of a container property is already carried by
// PropertyDescriptor::data_type; repeating it on the wire would waste one
// byte per container property.
namespace {
auto ReadDataTypeRef(BinaryReader& reader, std::size_t depth) -> std::optional<DataTypeRef>;
auto ReadDataTypeRefBody(BinaryReader& reader, PropertyDataType kind, std::size_t depth)
    -> std::optional<DataTypeRef>;

// Shared property-record reader. RegisterType (entity body) and
// RegisterComponent (component schema) emit identical property records;
// keeping one reader prevents the two paths from drifting.
bool ReadPropertyRecord(BinaryReader& reader, PropertyDescriptor& prop, std::string_view owner) {
  auto prop_name = reader.ReadString();
  auto data_type = reader.Read<uint8_t>();
  auto scope = reader.Read<uint8_t>();
  auto persistent = reader.Read<uint8_t>();
  auto detail_level = reader.Read<uint8_t>();
  auto index = reader.Read<uint16_t>();

  if (!prop_name || !data_type || !scope || !persistent || !detail_level || !index) {
    ATLAS_LOG_ERROR("read_property: header read failed for '{}'", owner);
    return false;
  }

  prop.name = std::move(*prop_name);
  prop.data_type = static_cast<PropertyDataType>(*data_type);
  if (prop.data_type == PropertyDataType::kInvalid) {
    ATLAS_LOG_ERROR("read_property: '{}.{}' has kInvalid data_type — generator bug", owner,
                    prop.name);
    return false;
  }
  prop.scope = static_cast<ReplicationScope>(*scope);
  prop.persistent = *persistent != 0;
  prop.detail_level = *detail_level;
  prop.index = *index;

  auto id_flag = reader.Read<uint8_t>();
  auto reliable_flag = reader.Read<uint8_t>();
  if (!id_flag || !reliable_flag) {
    ATLAS_LOG_ERROR("read_property: flags read failed for '{}.{}'", owner, prop.name);
    return false;
  }
  prop.identifier = *id_flag != 0;
  prop.reliable = *reliable_flag != 0;

  // Container properties carry `[DataTypeRef body] [PackedInt max_size]`.
  // The body skips its leading kind byte because prop.data_type already
  // pins the top-level kind; saving one byte per container property.
  const bool is_container = prop.data_type == PropertyDataType::kList ||
                            prop.data_type == PropertyDataType::kDict ||
                            prop.data_type == PropertyDataType::kStruct;
  if (is_container) {
    auto type_ref = ReadDataTypeRefBody(reader, prop.data_type, /*depth=*/0);
    if (!type_ref) {
      ATLAS_LOG_ERROR("read_property: type_ref read failed for '{}.{}'", owner, prop.name);
      return false;
    }
    prop.type_ref = std::move(*type_ref);

    auto max_size = reader.ReadPackedInt();
    if (!max_size) {
      ATLAS_LOG_ERROR("read_property: max_size read failed for '{}.{}'", owner, prop.name);
      return false;
    }
    prop.max_size = *max_size;
  }

  if (!ValidatePropertyInvariant(prop)) {
    ATLAS_LOG_ERROR(
        "read_property: '{}.{}' fails data_type/type_ref invariant "
        "(data_type={}, type_ref={})",
        owner, prop.name, static_cast<int>(prop.data_type),
        prop.type_ref.has_value() ? "present" : "absent");
    return false;
  }
  return true;
}
}  // namespace

EntityDefRegistry& EntityDefRegistry::Instance() {
  static EntityDefRegistry registry;
  return registry;
}

bool EntityDefRegistry::RegisterType(const std::byte* data, int32_t len) {
  if (data == nullptr || len <= 0) {
    ATLAS_LOG_ERROR("register_type: null data or zero length");
    return false;
  }

  BinaryReader reader(std::span<const std::byte>(data, static_cast<size_t>(len)));

  EntityTypeDescriptor desc;

  auto name_result = reader.ReadString();
  if (!name_result) {
    ATLAS_LOG_ERROR("register_type: failed to read type name");
    return false;
  }
  desc.name = std::move(*name_result);

  auto type_id_result = reader.Read<uint16_t>();
  if (!type_id_result) {
    ATLAS_LOG_ERROR("register_type: failed to read type_id for '{}'", desc.name);
    return false;
  }
  desc.type_id = *type_id_result;

  auto has_cell_result = reader.Read<uint8_t>();
  auto has_client_result = reader.Read<uint8_t>();
  if (!has_cell_result || !has_client_result) {
    ATLAS_LOG_ERROR("register_type: failed to read flags for '{}'", desc.name);
    return false;
  }
  desc.has_cell = *has_cell_result != 0;
  desc.has_client = *has_client_result != 0;

  auto prop_count_result = reader.ReadPackedInt();
  if (!prop_count_result) {
    ATLAS_LOG_ERROR("register_type: failed to read property count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *prop_count_result; ++i) {
    PropertyDescriptor prop;
    if (!ReadPropertyRecord(reader, prop, desc.name)) {
      ATLAS_LOG_ERROR("register_type: failed to read property {} for '{}'", i, desc.name);
      return false;
    }
    desc.properties.push_back(std::move(prop));
  }

  auto rpc_count_result = reader.ReadPackedInt();
  if (!rpc_count_result) {
    ATLAS_LOG_ERROR("register_type: failed to read RPC count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *rpc_count_result; ++i) {
    RpcDescriptor rpc;
    auto rpc_name = reader.ReadString();
    auto rpc_id = reader.ReadPackedInt();
    auto param_count = reader.ReadPackedInt();

    if (!rpc_name || !rpc_id || !param_count) {
      ATLAS_LOG_ERROR("register_type: failed to read RPC {} for '{}'", i, desc.name);
      return false;
    }

    rpc.name = std::move(*rpc_name);
    rpc.rpc_id = *rpc_id;

    for (uint32_t j = 0; j < *param_count; ++j) {
      auto pt = reader.Read<uint8_t>();
      if (!pt) {
        ATLAS_LOG_ERROR("register_type: failed to read RPC param type");
        return false;
      }
      rpc.param_types.push_back(static_cast<PropertyDataType>(*pt));
    }

    auto exposed_val = reader.Read<uint8_t>();
    if (exposed_val) {
      rpc.exposed = static_cast<ExposedScope>(*exposed_val);
    }

    desc.rpcs.push_back(std::move(rpc));
  }

  if (reader.Remaining() >= 2) {
    auto internal_comp = reader.Read<uint8_t>();
    auto external_comp = reader.Read<uint8_t>();
    if (internal_comp && external_comp) {
      desc.internal_compression = static_cast<EntityCompression>(*internal_comp);
      desc.external_compression = static_cast<EntityCompression>(*external_comp);
    }
  }

  // Any remaining bytes are the optional component slot section.
  if (reader.Remaining() > 0) {
    auto slot_count = reader.ReadPackedInt();
    if (!slot_count) {
      ATLAS_LOG_ERROR("register_type: failed to read slot count for '{}'", desc.name);
      return false;
    }
    for (uint32_t i = 0; i < *slot_count; ++i) {
      EntitySlotDescriptor slot;
      auto slot_idx = reader.Read<uint16_t>();
      auto slot_name = reader.ReadString();
      auto component_type_id = reader.Read<uint16_t>();
      auto slot_scope = reader.Read<uint8_t>();
      auto lazy = reader.Read<uint8_t>();
      if (!slot_idx || !slot_name || !component_type_id || !slot_scope || !lazy) {
        ATLAS_LOG_ERROR("register_type: failed to read slot {} for '{}'", i, desc.name);
        return false;
      }
      slot.slot_idx = *slot_idx;
      slot.slot_name = std::move(*slot_name);
      slot.component_type_id = *component_type_id;
      slot.scope = static_cast<ReplicationScope>(*slot_scope);
      slot.lazy = *lazy != 0;
      desc.slots.push_back(std::move(slot));
    }
  }

  // Warn about trailing bytes
  if (reader.Remaining() > 0) {
    ATLAS_LOG_WARNING("register_type: {} trailing bytes after '{}' descriptor", reader.Remaining(),
                      desc.name);
  }

  // Check for duplicate registration
  if (name_index.count(desc.name) > 0) {
    ATLAS_LOG_WARNING("register_type: duplicate type name '{}', replacing", desc.name);
  }
  if (id_index.count(desc.type_id) > 0) {
    ATLAS_LOG_WARNING("register_type: duplicate type_id {}, replacing", desc.type_id);
  }

  // Store
  auto idx = types.size();
  auto& type_name = desc.name;
  auto type_id = desc.type_id;

  ATLAS_LOG_INFO("Registered entity type '{}' (id={}, {} props, {} rpcs)", type_name, type_id,
                 desc.properties.size(), desc.rpcs.size());

  for (const auto& rpc : desc.rpcs) {
    rpc_to_type[rpc.rpc_id] = idx;
  }

  name_index[type_name] = idx;
  id_index[type_id] = idx;
  types.push_back(std::move(desc));

  return true;
}

const EntityTypeDescriptor* EntityDefRegistry::FindByName(std::string_view name) const {
  auto it = name_index.find(std::string(name));
  if (it == name_index.end()) return nullptr;
  return &types[it->second];
}

const EntityTypeDescriptor* EntityDefRegistry::FindById(uint16_t type_id) const {
  auto it = id_index.find(type_id);
  if (it == id_index.end()) return nullptr;
  return &types[it->second];
}

bool EntityDefRegistry::ValidateRpc(uint16_t type_id, uint32_t rpc_id) const {
  auto it = rpc_to_type.find(rpc_id);
  if (it == rpc_to_type.end()) return false;
  return types[it->second].type_id == type_id;
}

const RpcDescriptor* EntityDefRegistry::FindRpc(uint32_t rpc_id) const {
  auto it = rpc_to_type.find(rpc_id);
  if (it == rpc_to_type.end()) return nullptr;
  for (const auto& rpc : types[it->second].rpcs) {
    if (rpc.rpc_id == rpc_id) return &rpc;
  }
  return nullptr;
}

bool EntityDefRegistry::IsExposed(uint32_t rpc_id) const {
  auto* rpc = FindRpc(rpc_id);
  return rpc != nullptr && rpc->IsExposed();
}

ExposedScope EntityDefRegistry::GetExposedScope(uint32_t rpc_id) const {
  auto* rpc = FindRpc(rpc_id);
  return rpc ? rpc->exposed : ExposedScope::kNone;
}

std::vector<const PropertyDescriptor*> EntityDefRegistry::GetReplicatedProperties(
    uint16_t type_id, ReplicationScope min_scope) const {
  std::vector<const PropertyDescriptor*> result;
  auto it = id_index.find(type_id);
  if (it == id_index.end()) return result;

  for (const auto& prop : types[it->second].properties) {
    if (static_cast<uint8_t>(prop.scope) >= static_cast<uint8_t>(min_scope)) {
      result.push_back(&prop);
    }
  }
  return result;
}

std::vector<const PropertyDescriptor*> EntityDefRegistry::GetPersistentProperties(
    uint16_t type_id) const {
  std::vector<const PropertyDescriptor*> result;
  auto it = id_index.find(type_id);
  if (it == id_index.end()) return result;

  for (const auto& prop : types[it->second].properties) {
    if (prop.persistent) {
      result.push_back(&prop);
    }
  }
  return result;
}

void EntityDefRegistry::clear() {
  types.clear();
  name_index.clear();
  id_index.clear();
  rpc_to_type.clear();
  structs.clear();
  struct_id_index.clear();
  struct_name_index.clear();
  components.clear();
  component_id_index.clear();
  component_name_index.clear();
  ATLAS_LOG_INFO("EntityDefRegistry cleared");
}

namespace {

auto ReadDataTypeRefBody(BinaryReader& reader, PropertyDataType kind, std::size_t depth)
    -> std::optional<DataTypeRef> {
  if (depth > kMaxDataTypeDepth) {
    ATLAS_LOG_ERROR("data_type_ref: nesting exceeds max depth {}", kMaxDataTypeDepth);
    return std::nullopt;
  }

  DataTypeRef out;
  out.kind = kind;

  switch (kind) {
    case PropertyDataType::kBool:
    case PropertyDataType::kInt8:
    case PropertyDataType::kUInt8:
    case PropertyDataType::kInt16:
    case PropertyDataType::kUInt16:
    case PropertyDataType::kInt32:
    case PropertyDataType::kUInt32:
    case PropertyDataType::kInt64:
    case PropertyDataType::kUInt64:
    case PropertyDataType::kFloat:
    case PropertyDataType::kDouble:
    case PropertyDataType::kString:
    case PropertyDataType::kBytes:
    case PropertyDataType::kVector3:
    case PropertyDataType::kQuaternion:
    case PropertyDataType::kCustom:
      // Scalar: no trailing data.
      return out;

    case PropertyDataType::kList: {
      auto elem = ReadDataTypeRef(reader, depth + 1);
      if (!elem) return std::nullopt;
      out.elem = std::make_shared<DataTypeRef>(std::move(*elem));
      return out;
    }

    case PropertyDataType::kDict: {
      auto key = ReadDataTypeRef(reader, depth + 1);
      if (!key) return std::nullopt;
      auto val = ReadDataTypeRef(reader, depth + 1);
      if (!val) return std::nullopt;
      out.key = std::make_shared<DataTypeRef>(std::move(*key));
      out.elem = std::make_shared<DataTypeRef>(std::move(*val));
      return out;
    }

    case PropertyDataType::kStruct: {
      auto sid = reader.Read<uint16_t>();
      if (!sid) {
        ATLAS_LOG_ERROR("data_type_ref: kStruct missing struct_id");
        return std::nullopt;
      }
      out.struct_id = *sid;
      return out;
    }

    case PropertyDataType::kInvalid:
      ATLAS_LOG_ERROR("data_type_ref: kInvalid kind");
      return std::nullopt;
  }

  ATLAS_LOG_ERROR("data_type_ref: unknown kind {}", static_cast<int>(kind));
  return std::nullopt;
}

auto ReadDataTypeRef(BinaryReader& reader, std::size_t depth) -> std::optional<DataTypeRef> {
  auto kind_byte = reader.Read<uint8_t>();
  if (!kind_byte) {
    ATLAS_LOG_ERROR("data_type_ref: failed to read kind byte");
    return std::nullopt;
  }
  return ReadDataTypeRefBody(reader, static_cast<PropertyDataType>(*kind_byte), depth);
}

}  // namespace

bool EntityDefRegistry::RegisterStruct(const std::byte* data, int32_t len) {
  if (data == nullptr || len <= 0) {
    ATLAS_LOG_ERROR("register_struct: null data or zero length");
    return false;
  }

  BinaryReader reader(std::span<const std::byte>(data, static_cast<size_t>(len)));

  auto sid_result = reader.Read<uint16_t>();
  if (!sid_result) {
    ATLAS_LOG_ERROR("register_struct: failed to read struct_id");
    return false;
  }
  const uint16_t sid = *sid_result;

  auto name_result = reader.ReadString();
  if (!name_result) {
    ATLAS_LOG_ERROR("register_struct: failed to read name (struct_id={})", sid);
    return false;
  }

  StructDescriptor desc;
  desc.id = sid;
  desc.name = std::move(*name_result);

  auto field_count_result = reader.ReadPackedInt();
  if (!field_count_result) {
    ATLAS_LOG_ERROR("register_struct: failed to read field count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *field_count_result; ++i) {
    FieldDescriptor field;
    auto fname = reader.ReadString();
    if (!fname) {
      ATLAS_LOG_ERROR("register_struct: failed to read field {} name for '{}'", i, desc.name);
      return false;
    }
    field.name = std::move(*fname);
    auto type = ReadDataTypeRef(reader, /*depth=*/0);
    if (!type) {
      ATLAS_LOG_ERROR("register_struct: failed to read field '{}' type for '{}'", field.name,
                      desc.name);
      return false;
    }
    field.type = std::move(*type);
    desc.fields.push_back(std::move(field));
  }

  if (reader.Remaining() > 0) {
    ATLAS_LOG_WARNING("register_struct: {} trailing bytes after '{}'", reader.Remaining(),
                      desc.name);
  }

  // Collision handling: allow re-register of the same (id, name) as hot-reload,
  // but reject name/id mismatch which almost certainly means a bug in the
  // generator.
  if (auto it = struct_id_index.find(desc.id); it != struct_id_index.end()) {
    if (structs[it->second].name != desc.name) {
      ATLAS_LOG_ERROR("register_struct: id {} already bound to '{}', rejecting '{}'", desc.id,
                      structs[it->second].name, desc.name);
      return false;
    }
    ATLAS_LOG_WARNING("register_struct: replacing existing struct '{}' (id={})", desc.name,
                      desc.id);
    structs[it->second] = std::move(desc);
    return true;
  }
  if (auto it = struct_name_index.find(desc.name); it != struct_name_index.end()) {
    if (structs[it->second].id != desc.id) {
      ATLAS_LOG_ERROR("register_struct: name '{}' already bound to id {}, rejecting id {}",
                      desc.name, structs[it->second].id, desc.id);
      return false;
    }
    // An (id, name) pair that matches both would have returned in the id
    // branch above. Only the id-new / name-duplicate case reaches here,
    // which the `structs[it->second].id != desc.id` check catches and
    // rejects. No matching-match fallthrough is possible.
  }

  auto idx = structs.size();
  struct_id_index[desc.id] = idx;
  struct_name_index[desc.name] = idx;
  ATLAS_LOG_INFO("Registered struct '{}' (id={}, {} fields)", desc.name, desc.id,
                 desc.fields.size());
  structs.push_back(std::move(desc));
  return true;
}

const StructDescriptor* EntityDefRegistry::FindStructById(uint16_t struct_id) const {
  auto it = struct_id_index.find(struct_id);
  if (it == struct_id_index.end()) return nullptr;
  return &structs[it->second];
}

const StructDescriptor* EntityDefRegistry::FindStructByName(std::string_view name) const {
  auto it = struct_name_index.find(std::string(name));
  if (it == struct_name_index.end()) return nullptr;
  return &structs[it->second];
}

bool EntityDefRegistry::RegisterComponent(const std::byte* data, int32_t len) {
  if (data == nullptr || len <= 0) {
    ATLAS_LOG_ERROR("register_component: null data or zero length");
    return false;
  }

  BinaryReader reader(std::span<const std::byte>(data, static_cast<size_t>(len)));

  ComponentDescriptor desc;

  auto type_id_result = reader.Read<uint16_t>();
  if (!type_id_result) {
    ATLAS_LOG_ERROR("register_component: failed to read component_type_id");
    return false;
  }
  desc.component_type_id = *type_id_result;

  auto name_result = reader.ReadString();
  if (!name_result) {
    ATLAS_LOG_ERROR("register_component: failed to read name (id={})", desc.component_type_id);
    return false;
  }
  desc.name = std::move(*name_result);

  // Empty base_name (`""`) means "no inheritance". Resolution to a concrete
  // ComponentDescriptor* happens at attach time, not here, because the base
  // may be registered after this descriptor in a single startup batch.
  auto base_name_result = reader.ReadString();
  if (!base_name_result) {
    ATLAS_LOG_ERROR("register_component: failed to read base_name for '{}'", desc.name);
    return false;
  }
  desc.base_name = std::move(*base_name_result);

  auto locality_result = reader.Read<uint8_t>();
  if (!locality_result) {
    ATLAS_LOG_ERROR("register_component: failed to read locality for '{}'", desc.name);
    return false;
  }
  desc.locality = static_cast<ComponentLocality>(*locality_result);

  auto prop_count_result = reader.ReadPackedInt();
  if (!prop_count_result) {
    ATLAS_LOG_ERROR("register_component: failed to read property count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *prop_count_result; ++i) {
    PropertyDescriptor prop;
    if (!ReadPropertyRecord(reader, prop, desc.name)) {
      ATLAS_LOG_ERROR("register_component: failed to read property {} for '{}'", i, desc.name);
      return false;
    }
    desc.properties.push_back(std::move(prop));
  }

  if (reader.Remaining() > 0) {
    ATLAS_LOG_WARNING("register_component: {} trailing bytes after '{}'", reader.Remaining(),
                      desc.name);
  }

  // Same (id, name) may be re-registered; mismatched pairs are generator bugs.
  if (auto it = component_id_index.find(desc.component_type_id); it != component_id_index.end()) {
    if (components[it->second].name != desc.name) {
      ATLAS_LOG_ERROR("register_component: id {} already bound to '{}', rejecting '{}'",
                      desc.component_type_id, components[it->second].name, desc.name);
      return false;
    }
    ATLAS_LOG_WARNING("register_component: replacing existing component '{}' (id={})", desc.name,
                      desc.component_type_id);
    components[it->second] = std::move(desc);
    return true;
  }
  if (auto it = component_name_index.find(desc.name); it != component_name_index.end()) {
    if (components[it->second].component_type_id != desc.component_type_id) {
      ATLAS_LOG_ERROR("register_component: name '{}' already bound to id {}, rejecting id {}",
                      desc.name, components[it->second].component_type_id, desc.component_type_id);
      return false;
    }
  }

  auto idx = components.size();
  component_id_index[desc.component_type_id] = idx;
  component_name_index[desc.name] = idx;
  ATLAS_LOG_INFO("Registered component '{}' (id={}, base='{}', {} props, locality={})", desc.name,
                 desc.component_type_id, desc.base_name, desc.properties.size(),
                 static_cast<int>(desc.locality));
  components.push_back(std::move(desc));
  return true;
}

const ComponentDescriptor* EntityDefRegistry::FindComponentById(uint16_t component_type_id) const {
  auto it = component_id_index.find(component_type_id);
  if (it == component_id_index.end()) return nullptr;
  return &components[it->second];
}

const ComponentDescriptor* EntityDefRegistry::FindComponentByName(std::string_view name) const {
  auto it = component_name_index.find(std::string(name));
  if (it == component_name_index.end()) return nullptr;
  return &components[it->second];
}

namespace {

auto ReadAndRegisterRecord(BinaryReader& reader, std::string_view section,
                           const std::function<bool(const std::byte*, int32_t)>& register_fn)
    -> Result<void> {
  auto blob_len_result = reader.ReadPackedInt();
  if (!blob_len_result) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: failed to read blob length in ") +
                     std::string(section) + " section"};
  }
  auto blob_len = *blob_len_result;
  auto bytes_result = reader.ReadBytes(blob_len);
  if (!bytes_result) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: failed to read blob bytes in ") +
                     std::string(section) + " section"};
  }
  if (!register_fn(bytes_result->data(), static_cast<int32_t>(blob_len))) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: register failed for ") +
                     std::string(section) + " record"};
  }
  return {};
}

}  // namespace

auto EntityDefRegistry::RegisterFromBinaryFile(const std::filesystem::path& path)
    -> Result<LoadedCounts> {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: cannot open ") + path.string()};
  }
  std::streamsize size = f.tellg();
  if (size < 0) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryFile: tellg failed for ") + path.string()};
  }
  std::vector<std::byte> buf(static_cast<size_t>(size));
  f.seekg(0, std::ios::beg);
  if (size > 0) {
    f.read(reinterpret_cast<char*>(buf.data()), size);
    if (f.gcount() != size) {
      return Error{ErrorCode::kInvalidArgument,
                   std::string("RegisterFromBinaryFile: short read for ") + path.string()};
    }
  }
  return RegisterFromBinaryBuffer(std::span<const std::byte>(buf.data(), buf.size()));
}

auto EntityDefRegistry::RegisterFromBinaryBuffer(std::span<const std::byte> buf)
    -> Result<LoadedCounts> {
  BinaryReader reader(buf);

  auto magic_result = reader.Read<uint32_t>();
  if (!magic_result || *magic_result != kBinaryFileMagic) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: bad magic (expected 'ATDF')"};
  }
  auto version_result = reader.Read<uint16_t>();
  if (!version_result || *version_result != kBinaryFileVersion) {
    return Error{ErrorCode::kInvalidArgument,
                 std::string("RegisterFromBinaryBuffer: unsupported version ") +
                     std::to_string(version_result.HasValue() ? *version_result : 0)};
  }
  auto flags_result = reader.Read<uint16_t>();
  if (!flags_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read flags word"};
  }
  (void)*flags_result;

  LoadedCounts out;

  // Section order: structs -> components -> types. References resolve in
  // this order: types reference component_type_id (must exist), and
  // components / entity properties may reference struct_id (must exist).
  auto struct_count_result = reader.ReadPackedInt();
  if (!struct_count_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read struct count"};
  }
  for (uint32_t i = 0; i < *struct_count_result; ++i) {
    auto r = ReadAndRegisterRecord(
        reader, "struct", [this](const std::byte* d, int32_t n) { return RegisterStruct(d, n); });
    if (!r) return r.Error();
  }
  out.structs = *struct_count_result;

  auto component_count_result = reader.ReadPackedInt();
  if (!component_count_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read component count"};
  }
  for (uint32_t i = 0; i < *component_count_result; ++i) {
    auto r = ReadAndRegisterRecord(reader, "component", [this](const std::byte* d, int32_t n) {
      return RegisterComponent(d, n);
    });
    if (!r) return r.Error();
  }
  out.components = *component_count_result;

  auto type_count_result = reader.ReadPackedInt();
  if (!type_count_result) {
    return Error{ErrorCode::kInvalidArgument,
                 "RegisterFromBinaryBuffer: failed to read type count"};
  }
  for (uint32_t i = 0; i < *type_count_result; ++i) {
    auto r = ReadAndRegisterRecord(
        reader, "type", [this](const std::byte* d, int32_t n) { return RegisterType(d, n); });
    if (!r) return r.Error();
  }
  out.types = *type_count_result;

  if (reader.Remaining() > 0) {
    ATLAS_LOG_WARNING("RegisterFromBinaryBuffer: {} trailing bytes after type section",
                      reader.Remaining());
  }
  return out;
}

// Minimal self-contained MD5 (RFC 1321), used only for descriptor validation.
namespace detail {

struct Md5Ctx {
  uint32_t state[4]{0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476};
  uint64_t count{0};
  uint8_t buf[64]{};
};

// clang-format off
static constexpr uint32_t kS[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21,
};
static constexpr uint32_t kK[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
    0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
    0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
    0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
    0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
    0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
    0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
    0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
    0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
};
// clang-format on

static auto Rotl32(uint32_t x, uint32_t n) -> uint32_t {
  return (x << n) | (x >> (32 - n));
}

static void Md5Transform(uint32_t state[4], const uint8_t block[64]) {
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
  uint32_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = static_cast<uint32_t>(block[i * 4]) | (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
           (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
           (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
  }
  for (int i = 0; i < 64; ++i) {
    uint32_t f, g;
    if (i < 16) {
      f = (b & c) | (~b & d);
      g = static_cast<uint32_t>(i);
    } else if (i < 32) {
      f = (d & b) | (~d & c);
      g = static_cast<uint32_t>(5 * i + 1) % 16;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = static_cast<uint32_t>(3 * i + 5) % 16;
    } else {
      f = c ^ (b | ~d);
      g = static_cast<uint32_t>(7 * i) % 16;
    }
    uint32_t temp = d;
    d = c;
    c = b;
    b = b + Rotl32(a + f + kK[i] + m[g], kS[i]);
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
}

static void Md5Update(Md5Ctx& ctx, const uint8_t* data, size_t len) {
  size_t offset = static_cast<size_t>(ctx.count % 64);
  ctx.count += len;
  size_t avail = 64 - offset;
  if (len >= avail) {
    std::memcpy(ctx.buf + offset, data, avail);
    Md5Transform(ctx.state, ctx.buf);
    data += avail;
    len -= avail;
    while (len >= 64) {
      Md5Transform(ctx.state, data);
      data += 64;
      len -= 64;
    }
    offset = 0;
  }
  std::memcpy(ctx.buf + offset, data, len);
}

static auto Md5Final(Md5Ctx& ctx) -> std::array<uint8_t, 16> {
  uint64_t bits = ctx.count * 8;
  uint8_t pad = 0x80;
  Md5Update(ctx, &pad, 1);
  pad = 0;
  while (ctx.count % 64 != 56) Md5Update(ctx, &pad, 1);
  uint8_t bits_le[8];
  for (int i = 0; i < 8; ++i) bits_le[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
  Md5Update(ctx, bits_le, 8);

  std::array<uint8_t, 16> digest{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      digest[static_cast<size_t>(i * 4 + j)] =
          static_cast<uint8_t>((ctx.state[static_cast<size_t>(i)] >> (j * 8)) & 0xFF);
  return digest;
}

}  // namespace detail

auto EntityDefRegistry::PersistentPropertiesDigest() const -> std::array<uint8_t, 16> {
  detail::Md5Ctx ctx;

  // Feed sorted types (by type_id) for determinism
  auto sorted = types;
  std::sort(sorted.begin(), sorted.end(),
            [](const EntityTypeDescriptor& a, const EntityTypeDescriptor& b) {
              return a.type_id < b.type_id;
            });

  for (const auto& type : sorted) {
    uint16_t tid = type.type_id;
    detail::Md5Update(ctx, reinterpret_cast<const uint8_t*>(&tid), 2);
    detail::Md5Update(ctx, reinterpret_cast<const uint8_t*>(type.name.data()), type.name.size());

    for (const auto& prop : type.properties) {
      if (!prop.persistent) continue;
      detail::Md5Update(ctx, reinterpret_cast<const uint8_t*>(prop.name.data()), prop.name.size());
      auto dt = static_cast<uint8_t>(prop.data_type);
      detail::Md5Update(ctx, &dt, 1);
      auto id = static_cast<uint8_t>(prop.identifier ? 1 : 0);
      detail::Md5Update(ctx, &id, 1);
    }
  }

  return detail::Md5Final(ctx);
}

}  // namespace atlas
