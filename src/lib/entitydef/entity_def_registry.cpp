#include "entitydef/entity_def_registry.hpp"

#include "foundation/log.hpp"
#include "serialization/binary_stream.hpp"
#include "serialization/data_section.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>

namespace atlas
{

EntityDefRegistry& EntityDefRegistry::instance()
{
    static EntityDefRegistry registry;
    return registry;
}

bool EntityDefRegistry::register_type(const std::byte* data, int32_t len)
{
    if (data == nullptr || len <= 0)
    {
        ATLAS_LOG_ERROR("register_type: null data or zero length");
        return false;
    }

    BinaryReader reader(std::span<const std::byte>(data, static_cast<size_t>(len)));

    EntityTypeDescriptor desc;

    // Type header
    auto name_result = reader.read_string();
    if (!name_result)
    {
        ATLAS_LOG_ERROR("register_type: failed to read type name");
        return false;
    }
    desc.name = std::move(*name_result);

    auto type_id_result = reader.read<uint16_t>();
    if (!type_id_result)
    {
        ATLAS_LOG_ERROR("register_type: failed to read type_id for '{}'", desc.name);
        return false;
    }
    desc.type_id = *type_id_result;

    auto has_cell_result = reader.read<uint8_t>();
    auto has_client_result = reader.read<uint8_t>();
    if (!has_cell_result || !has_client_result)
    {
        ATLAS_LOG_ERROR("register_type: failed to read flags for '{}'", desc.name);
        return false;
    }
    desc.has_cell = *has_cell_result != 0;
    desc.has_client = *has_client_result != 0;

    // Properties
    auto prop_count_result = reader.read_packed_int();
    if (!prop_count_result)
    {
        ATLAS_LOG_ERROR("register_type: failed to read property count for '{}'", desc.name);
        return false;
    }

    for (uint32_t i = 0; i < *prop_count_result; ++i)
    {
        PropertyDescriptor prop;
        auto prop_name = reader.read_string();
        auto data_type = reader.read<uint8_t>();
        auto scope = reader.read<uint8_t>();
        auto persistent = reader.read<uint8_t>();
        auto detail_level = reader.read<uint8_t>();
        auto index = reader.read<uint16_t>();

        if (!prop_name || !data_type || !scope || !persistent || !detail_level || !index)
        {
            ATLAS_LOG_ERROR("register_type: failed to read property {} for '{}'", i, desc.name);
            return false;
        }

        prop.name = std::move(*prop_name);
        prop.data_type = static_cast<PropertyDataType>(*data_type);
        prop.scope = static_cast<ReplicationScope>(*scope);
        prop.persistent = *persistent != 0;
        prop.detail_level = *detail_level;
        prop.index = *index;

        // identifier flag — optional field added in protocol v2 (absent = false)
        if (reader.remaining() >= 1)
        {
            // Peek: only consume if the next byte looks like a flag (0 or 1)
            // Use a soft read to stay forward-compatible.
            auto id_flag = reader.read<uint8_t>();
            if (id_flag)
                prop.identifier = *id_flag != 0;
        }

        desc.properties.push_back(std::move(prop));
    }

    // RPCs
    auto rpc_count_result = reader.read_packed_int();
    if (!rpc_count_result)
    {
        ATLAS_LOG_ERROR("register_type: failed to read RPC count for '{}'", desc.name);
        return false;
    }

    for (uint32_t i = 0; i < *rpc_count_result; ++i)
    {
        RpcDescriptor rpc;
        auto rpc_name = reader.read_string();
        auto rpc_id = reader.read_packed_int();
        auto param_count = reader.read_packed_int();

        if (!rpc_name || !rpc_id || !param_count)
        {
            ATLAS_LOG_ERROR("register_type: failed to read RPC {} for '{}'", i, desc.name);
            return false;
        }

        rpc.name = std::move(*rpc_name);
        rpc.rpc_id = *rpc_id;

        for (uint32_t j = 0; j < *param_count; ++j)
        {
            auto pt = reader.read<uint8_t>();
            if (!pt)
            {
                ATLAS_LOG_ERROR("register_type: failed to read RPC param type");
                return false;
            }
            rpc.param_types.push_back(static_cast<PropertyDataType>(*pt));
        }

        // ExposedScope — appended after param_types
        auto exposed_val = reader.read<uint8_t>();
        if (exposed_val)
        {
            rpc.exposed = static_cast<ExposedScope>(*exposed_val);
        }

        desc.rpcs.push_back(std::move(rpc));
    }

    // Compression settings (optional — absent in older descriptors)
    if (reader.remaining() >= 2)
    {
        auto internal_comp = reader.read<uint8_t>();
        auto external_comp = reader.read<uint8_t>();
        if (internal_comp && external_comp)
        {
            desc.internal_compression = static_cast<EntityCompression>(*internal_comp);
            desc.external_compression = static_cast<EntityCompression>(*external_comp);
        }
    }

    // Warn about trailing bytes
    if (reader.remaining() > 0)
    {
        ATLAS_LOG_WARNING("register_type: {} trailing bytes after '{}' descriptor",
                          reader.remaining(), desc.name);
    }

    // Check for duplicate registration
    if (name_index_.count(desc.name) > 0)
    {
        ATLAS_LOG_WARNING("register_type: duplicate type name '{}', replacing", desc.name);
    }
    if (id_index_.count(desc.type_id) > 0)
    {
        ATLAS_LOG_WARNING("register_type: duplicate type_id {}, replacing", desc.type_id);
    }

    // Store
    auto idx = types_.size();
    auto& type_name = desc.name;
    auto type_id = desc.type_id;

    ATLAS_LOG_INFO("Registered entity type '{}' (id={}, {} props, {} rpcs)", type_name, type_id,
                   desc.properties.size(), desc.rpcs.size());

    for (const auto& rpc : desc.rpcs)
    {
        rpc_to_type_[rpc.rpc_id] = idx;
    }

    name_index_[type_name] = idx;
    id_index_[type_id] = idx;
    types_.push_back(std::move(desc));

    return true;
}

const EntityTypeDescriptor* EntityDefRegistry::find_by_name(std::string_view name) const
{
    auto it = name_index_.find(std::string(name));
    if (it == name_index_.end())
        return nullptr;
    return &types_[it->second];
}

const EntityTypeDescriptor* EntityDefRegistry::find_by_id(uint16_t type_id) const
{
    auto it = id_index_.find(type_id);
    if (it == id_index_.end())
        return nullptr;
    return &types_[it->second];
}

bool EntityDefRegistry::validate_rpc(uint16_t type_id, uint32_t rpc_id) const
{
    auto it = rpc_to_type_.find(rpc_id);
    if (it == rpc_to_type_.end())
        return false;
    return types_[it->second].type_id == type_id;
}

const RpcDescriptor* EntityDefRegistry::find_rpc(uint32_t rpc_id) const
{
    auto it = rpc_to_type_.find(rpc_id);
    if (it == rpc_to_type_.end())
        return nullptr;
    for (const auto& rpc : types_[it->second].rpcs)
    {
        if (rpc.rpc_id == rpc_id)
            return &rpc;
    }
    return nullptr;
}

bool EntityDefRegistry::is_exposed(uint32_t rpc_id) const
{
    auto* rpc = find_rpc(rpc_id);
    return rpc != nullptr && rpc->is_exposed();
}

ExposedScope EntityDefRegistry::get_exposed_scope(uint32_t rpc_id) const
{
    auto* rpc = find_rpc(rpc_id);
    return rpc ? rpc->exposed : ExposedScope::None;
}

std::vector<const PropertyDescriptor*> EntityDefRegistry::get_replicated_properties(
    uint16_t type_id, ReplicationScope min_scope) const
{
    std::vector<const PropertyDescriptor*> result;
    auto it = id_index_.find(type_id);
    if (it == id_index_.end())
        return result;

    for (const auto& prop : types_[it->second].properties)
    {
        if (static_cast<uint8_t>(prop.scope) >= static_cast<uint8_t>(min_scope))
        {
            result.push_back(&prop);
        }
    }
    return result;
}

std::vector<const PropertyDescriptor*> EntityDefRegistry::get_persistent_properties(
    uint16_t type_id) const
{
    std::vector<const PropertyDescriptor*> result;
    auto it = id_index_.find(type_id);
    if (it == id_index_.end())
        return result;

    for (const auto& prop : types_[it->second].properties)
    {
        if (prop.persistent)
        {
            result.push_back(&prop);
        }
    }
    return result;
}

void EntityDefRegistry::clear()
{
    types_.clear();
    name_index_.clear();
    id_index_.clear();
    rpc_to_type_.clear();
    ATLAS_LOG_INFO("EntityDefRegistry cleared");
}

// ============================================================================
// from_json_file — load entity definitions from entity_defs.json
// ============================================================================

auto EntityDefRegistry::from_json_file(const std::filesystem::path& path)
    -> Result<EntityDefRegistry>
{
    auto tree_result = DataSection::from_json(path);
    if (!tree_result)
    {
        return tree_result.error();
    }
    auto* root = (*tree_result)->root();
    if (root == nullptr)
    {
        return Error{ErrorCode::InvalidArgument, "entity_defs.json: empty document"};
    }

    // Root must have a "types" array child
    auto* types_node = root->child("types");
    if (types_node == nullptr)
    {
        return Error{ErrorCode::InvalidArgument, "entity_defs.json: missing 'types' array"};
    }

    EntityDefRegistry registry;

    for (auto* type_node : types_node->children())
    {
        EntityTypeDescriptor desc;
        desc.type_id = static_cast<uint16_t>(type_node->read_uint("type_id", 0));
        desc.name = type_node->read_string("name");
        desc.has_cell = type_node->read_bool("has_cell", false);
        desc.has_client = type_node->read_bool("has_client", false);

        auto* props_node = type_node->child("properties");
        if (props_node != nullptr)
        {
            for (auto* prop_node : props_node->children())
            {
                PropertyDescriptor prop;
                prop.name = prop_node->read_string("name");
                prop.persistent = prop_node->read_bool("persistent", false);
                prop.identifier = prop_node->read_bool("identifier", false);
                prop.index = static_cast<uint16_t>(prop_node->read_uint("index", 0));

                // data_type from string
                auto type_str = prop_node->read_string("type");
                if (type_str == "bool")
                    prop.data_type = PropertyDataType::Bool;
                else if (type_str == "int8")
                    prop.data_type = PropertyDataType::Int8;
                else if (type_str == "uint8")
                    prop.data_type = PropertyDataType::UInt8;
                else if (type_str == "int16")
                    prop.data_type = PropertyDataType::Int16;
                else if (type_str == "uint16")
                    prop.data_type = PropertyDataType::UInt16;
                else if (type_str == "int32")
                    prop.data_type = PropertyDataType::Int32;
                else if (type_str == "uint32")
                    prop.data_type = PropertyDataType::UInt32;
                else if (type_str == "int64")
                    prop.data_type = PropertyDataType::Int64;
                else if (type_str == "uint64")
                    prop.data_type = PropertyDataType::UInt64;
                else if (type_str == "float")
                    prop.data_type = PropertyDataType::Float;
                else if (type_str == "double")
                    prop.data_type = PropertyDataType::Double;
                else if (type_str == "string")
                    prop.data_type = PropertyDataType::String;
                else if (type_str == "bytes")
                    prop.data_type = PropertyDataType::Bytes;
                else if (type_str == "vector3")
                    prop.data_type = PropertyDataType::Vector3;
                else if (type_str == "quaternion")
                    prop.data_type = PropertyDataType::Quaternion;
                else
                    prop.data_type = PropertyDataType::Custom;

                // scope
                auto scope_str = prop_node->read_string("scope", "cell_private");
                if (scope_str == "cell_public")
                    prop.scope = ReplicationScope::CellPublic;
                else if (scope_str == "own_client")
                    prop.scope = ReplicationScope::OwnClient;
                else if (scope_str == "other_clients")
                    prop.scope = ReplicationScope::OtherClients;
                else if (scope_str == "all_clients")
                    prop.scope = ReplicationScope::AllClients;
                else if (scope_str == "cell_public_and_own")
                    prop.scope = ReplicationScope::CellPublicAndOwn;
                else if (scope_str == "base" || scope_str == "base_only")
                    prop.scope = ReplicationScope::Base;
                else if (scope_str == "base_and_client")
                    prop.scope = ReplicationScope::BaseAndClient;
                else
                    prop.scope = ReplicationScope::CellPrivate;

                prop.detail_level = static_cast<uint8_t>(prop_node->read_uint("detail_level", 5));
                desc.properties.push_back(std::move(prop));
            }
        }

        if (desc.name.empty() || desc.type_id == 0)
        {
            ATLAS_LOG_WARNING("from_json_file: skipping invalid type entry (name='{}', id={})",
                              desc.name, desc.type_id);
            continue;
        }

        auto idx = registry.types_.size();
        for (const auto& rpc : desc.rpcs)
            registry.rpc_to_type_[rpc.rpc_id] = idx;
        registry.name_index_[desc.name] = idx;
        registry.id_index_[desc.type_id] = idx;

        ATLAS_LOG_INFO("from_json_file: loaded type '{}' (id={}, {} props)", desc.name,
                       desc.type_id, desc.properties.size());
        registry.types_.push_back(std::move(desc));
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

namespace detail
{

struct Md5Ctx
{
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

static auto rotl32(uint32_t x, uint32_t n) -> uint32_t
{
    return (x << n) | (x >> (32 - n));
}

static void md5_transform(uint32_t state[4], const uint8_t block[64])
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t M[16];
    for (int i = 0; i < 16; ++i)
    {
        M[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }
    for (int i = 0; i < 64; ++i)
    {
        uint32_t F, g;
        if (i < 16)
        {
            F = (b & c) | (~b & d);
            g = static_cast<uint32_t>(i);
        }
        else if (i < 32)
        {
            F = (d & b) | (~d & c);
            g = static_cast<uint32_t>(5 * i + 1) % 16;
        }
        else if (i < 48)
        {
            F = b ^ c ^ d;
            g = static_cast<uint32_t>(3 * i + 5) % 16;
        }
        else
        {
            F = c ^ (b | ~d);
            g = static_cast<uint32_t>(7 * i) % 16;
        }
        uint32_t temp = d;
        d = c;
        c = b;
        b = b + rotl32(a + F + kK[i] + M[g], kS[i]);
        a = temp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void md5_update(Md5Ctx& ctx, const uint8_t* data, size_t len)
{
    size_t offset = static_cast<size_t>(ctx.count % 64);
    ctx.count += len;
    size_t avail = 64 - offset;
    if (len >= avail)
    {
        std::memcpy(ctx.buf + offset, data, avail);
        md5_transform(ctx.state, ctx.buf);
        data += avail;
        len -= avail;
        while (len >= 64)
        {
            md5_transform(ctx.state, data);
            data += 64;
            len -= 64;
        }
        offset = 0;
    }
    std::memcpy(ctx.buf + offset, data, len);
}

static auto md5_final(Md5Ctx& ctx) -> std::array<uint8_t, 16>
{
    uint64_t bits = ctx.count * 8;
    uint8_t pad = 0x80;
    md5_update(ctx, &pad, 1);
    pad = 0;
    while (ctx.count % 64 != 56)
        md5_update(ctx, &pad, 1);
    uint8_t bits_le[8];
    for (int i = 0; i < 8; ++i)
        bits_le[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
    md5_update(ctx, bits_le, 8);

    std::array<uint8_t, 16> digest{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            digest[i * 4 + j] = static_cast<uint8_t>((ctx.state[i] >> (j * 8)) & 0xFF);
    return digest;
}

}  // namespace detail

auto EntityDefRegistry::persistent_properties_digest() const -> std::array<uint8_t, 16>
{
    detail::Md5Ctx ctx;

    // Feed sorted types (by type_id) for determinism
    auto sorted = types_;
    std::sort(sorted.begin(), sorted.end(),
              [](const EntityTypeDescriptor& a, const EntityTypeDescriptor& b)
              { return a.type_id < b.type_id; });

    for (const auto& type : sorted)
    {
        uint16_t tid = type.type_id;
        detail::md5_update(ctx, reinterpret_cast<const uint8_t*>(&tid), 2);
        detail::md5_update(ctx, reinterpret_cast<const uint8_t*>(type.name.data()),
                           type.name.size());

        for (const auto& prop : type.properties)
        {
            if (!prop.persistent)
                continue;
            detail::md5_update(ctx, reinterpret_cast<const uint8_t*>(prop.name.data()),
                               prop.name.size());
            auto dt = static_cast<uint8_t>(prop.data_type);
            detail::md5_update(ctx, &dt, 1);
            auto id = static_cast<uint8_t>(prop.identifier ? 1 : 0);
            detail::md5_update(ctx, &id, 1);
        }
    }

    return detail::md5_final(ctx);
}

}  // namespace atlas
