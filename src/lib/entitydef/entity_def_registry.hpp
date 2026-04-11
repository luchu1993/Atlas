#pragma once

#include "entitydef/entity_type_descriptor.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace atlas
{

/// Global registry of entity type descriptors.
/// Populated at startup by C# EntityTypeRegistry.RegisterAll() → NativeApi → here.
/// Replaces the traditional .def XML file parsing.
class EntityDefRegistry
{
public:
    static EntityDefRegistry& instance();

    /// Deserialize a binary entity type descriptor from C# and register it.
    /// Returns true on success.
    bool register_type(const std::byte* data, int32_t len);

    /// Find descriptor by type name.
    [[nodiscard]] const EntityTypeDescriptor* find_by_name(std::string_view name) const;

    /// Find descriptor by type ID.
    [[nodiscard]] const EntityTypeDescriptor* find_by_id(uint16_t type_id) const;

    /// Validate that rpc_id belongs to the given entity type.
    [[nodiscard]] bool validate_rpc(uint16_t type_id, uint32_t rpc_id) const;

    /// Get replicated properties for a type, filtered by max_scope.
    /// Returns properties whose scope >= max_scope threshold appropriate for the target:
    ///   - For owner client: scope >= OwnClient (2)
    ///   - For other clients: scope >= AllClients (3)
    [[nodiscard]] std::vector<const PropertyDescriptor*> get_replicated_properties(
        uint16_t type_id, ReplicationScope min_scope) const;

    /// Get all persistent properties for a type.
    [[nodiscard]] std::vector<const PropertyDescriptor*> get_persistent_properties(
        uint16_t type_id) const;

    /// Number of registered types.
    [[nodiscard]] size_t type_count() const { return types_.size(); }

    /// Clear all registered types (used during hot-reload).
    void clear();

private:
    EntityDefRegistry() = default;

    std::vector<EntityTypeDescriptor> types_;
    std::unordered_map<std::string, size_t> name_index_;
    std::unordered_map<uint16_t, size_t> id_index_;
    // Maps rpc_id → type index for fast RPC validation
    std::unordered_map<uint32_t, size_t> rpc_to_type_;
};

}  // namespace atlas
