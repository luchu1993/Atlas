#include "entitydef/entity_def_registry.hpp"

#include "foundation/log.hpp"
#include "serialization/binary_stream.hpp"

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

}  // namespace atlas
