#include "entitydef/entity_def_registry.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>

#include "foundation/log.h"
#include "serialization/binary_stream.h"
#include "serialization/data_section.h"

namespace atlas {

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

  // Type header
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

  // Properties
  auto prop_count_result = reader.ReadPackedInt();
  if (!prop_count_result) {
    ATLAS_LOG_ERROR("register_type: failed to read property count for '{}'", desc.name);
    return false;
  }

  for (uint32_t i = 0; i < *prop_count_result; ++i) {
    PropertyDescriptor prop;
    auto prop_name = reader.ReadString();
    auto data_type = reader.Read<uint8_t>();
    auto scope = reader.Read<uint8_t>();
    auto persistent = reader.Read<uint8_t>();
    auto detail_level = reader.Read<uint8_t>();
    auto index = reader.Read<uint16_t>();

    if (!prop_name || !data_type || !scope || !persistent || !detail_level || !index) {
      ATLAS_LOG_ERROR("register_type: failed to read property {} for '{}'", i, desc.name);
      return false;
    }

    prop.name = std::move(*prop_name);
    prop.data_type = static_cast<PropertyDataType>(*data_type);
    prop.scope = static_cast<ReplicationScope>(*scope);
    prop.persistent = *persistent != 0;
    prop.detail_level = *detail_level;
    prop.index = *index;

    // Protocol v2: identifier + reliable flags are emitted explicitly per property.
    auto id_flag = reader.Read<uint8_t>();
    auto reliable_flag = reader.Read<uint8_t>();
    if (!id_flag || !reliable_flag) {
      ATLAS_LOG_ERROR("register_type: failed to read property flags for '{}'", desc.name);
      return false;
    }
    prop.identifier = *id_flag != 0;
    prop.reliable = *reliable_flag != 0;

    desc.properties.push_back(std::move(prop));
  }

  // RPCs
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

    // ExposedScope — appended after param_types
    auto exposed_val = reader.Read<uint8_t>();
    if (exposed_val) {
      rpc.exposed = static_cast<ExposedScope>(*exposed_val);
    }

    desc.rpcs.push_back(std::move(rpc));
  }

  // Compression settings (optional — absent in older descriptors)
  if (reader.Remaining() >= 2) {
    auto internal_comp = reader.Read<uint8_t>();
    auto external_comp = reader.Read<uint8_t>();
    if (internal_comp && external_comp) {
      desc.internal_compression = static_cast<EntityCompression>(*internal_comp);
      desc.external_compression = static_cast<EntityCompression>(*external_comp);
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
  ATLAS_LOG_INFO("EntityDefRegistry cleared");
}

// ============================================================================
// from_json_file — load entity definitions from entity_defs.json
// ============================================================================

auto EntityDefRegistry::FromJsonFile(const std::filesystem::path& path)
    -> Result<EntityDefRegistry> {
  auto tree_result = DataSection::FromJson(path);
  if (!tree_result) {
    return tree_result.Error();
  }
  auto* root = (*tree_result)->Root();
  if (root == nullptr) {
    return Error{ErrorCode::kInvalidArgument, "entity_defs.json: empty document"};
  }

  // Root must have a "types" array child
  auto* typesnode = root->Child("types");
  if (typesnode == nullptr) {
    return Error{ErrorCode::kInvalidArgument, "entity_defs.json: missing 'types' array"};
  }

  EntityDefRegistry registry;

  for (auto* type_node : typesnode->Children()) {
    EntityTypeDescriptor desc;
    desc.type_id = static_cast<uint16_t>(type_node->ReadUint("type_id", 0));
    desc.name = type_node->ReadString("name");
    desc.has_cell = type_node->ReadBool("has_cell", false);
    desc.has_client = type_node->ReadBool("has_client", false);

    auto* props_node = type_node->Child("properties");
    if (props_node != nullptr) {
      for (auto* prop_node : props_node->Children()) {
        PropertyDescriptor prop;
        prop.name = prop_node->ReadString("name");
        prop.persistent = prop_node->ReadBool("persistent", false);
        prop.identifier = prop_node->ReadBool("identifier", false);
        prop.reliable = prop_node->ReadBool("reliable", false);
        prop.index = static_cast<uint16_t>(prop_node->ReadUint("index", 0));

        // data_type from string
        auto type_str = prop_node->ReadString("type");
        if (type_str == "bool")
          prop.data_type = PropertyDataType::kBool;
        else if (type_str == "int8")
          prop.data_type = PropertyDataType::kInt8;
        else if (type_str == "uint8")
          prop.data_type = PropertyDataType::kUInt8;
        else if (type_str == "int16")
          prop.data_type = PropertyDataType::kInt16;
        else if (type_str == "uint16")
          prop.data_type = PropertyDataType::kUInt16;
        else if (type_str == "int32")
          prop.data_type = PropertyDataType::kInt32;
        else if (type_str == "uint32")
          prop.data_type = PropertyDataType::kUInt32;
        else if (type_str == "int64")
          prop.data_type = PropertyDataType::kInt64;
        else if (type_str == "uint64")
          prop.data_type = PropertyDataType::kUInt64;
        else if (type_str == "float")
          prop.data_type = PropertyDataType::kFloat;
        else if (type_str == "double")
          prop.data_type = PropertyDataType::kDouble;
        else if (type_str == "string")
          prop.data_type = PropertyDataType::kString;
        else if (type_str == "bytes")
          prop.data_type = PropertyDataType::kBytes;
        else if (type_str == "vector3")
          prop.data_type = PropertyDataType::kVector3;
        else if (type_str == "quaternion")
          prop.data_type = PropertyDataType::kQuaternion;
        else
          prop.data_type = PropertyDataType::kCustom;

        // scope
        auto scope_str = prop_node->ReadString("scope", "cell_private");
        if (scope_str == "cell_public")
          prop.scope = ReplicationScope::kCellPublic;
        else if (scope_str == "own_client")
          prop.scope = ReplicationScope::kOwnClient;
        else if (scope_str == "other_clients")
          prop.scope = ReplicationScope::kOtherClients;
        else if (scope_str == "all_clients")
          prop.scope = ReplicationScope::kAllClients;
        else if (scope_str == "cell_public_and_own")
          prop.scope = ReplicationScope::kCellPublicAndOwn;
        else if (scope_str == "base" || scope_str == "base_only")
          prop.scope = ReplicationScope::kBase;
        else if (scope_str == "base_and_client")
          prop.scope = ReplicationScope::kBaseAndClient;
        else
          prop.scope = ReplicationScope::kCellPrivate;

        prop.detail_level = static_cast<uint8_t>(prop_node->ReadUint("detail_level", 5));
        desc.properties.push_back(std::move(prop));
      }
    }

    if (desc.name.empty() || desc.type_id == 0) {
      ATLAS_LOG_WARNING("from_json_file: skipping invalid type entry (name='{}', id={})", desc.name,
                        desc.type_id);
      continue;
    }

    auto idx = registry.types.size();
    for (const auto& rpc : desc.rpcs) registry.rpc_to_type[rpc.rpc_id] = idx;
    registry.name_index[desc.name] = idx;
    registry.id_index[desc.type_id] = idx;

    ATLAS_LOG_INFO("from_json_file: loaded type '{}' (id={}, {} props)", desc.name, desc.type_id,
                   desc.properties.size());
    registry.types.push_back(std::move(desc));
  }

  return registry;
}

// ============================================================================
// persistent_properties_digest — MD5 over stable descriptor fields
// ============================================================================
//
// Minimal self-contained MD5 (RFC 1321).  Only used for descriptor validation;
// no security requirements.
// ============================================================================

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
