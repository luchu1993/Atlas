#ifndef ATLAS_LIB_ENTITYDEF_ENTITY_DEF_REGISTRY_H_
#define ATLAS_LIB_ENTITYDEF_ENTITY_DEF_REGISTRY_H_

#include <array>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "entitydef/entity_type_descriptor.h"
#include "foundation/error.h"

namespace atlas {

/// Global registry of entity type descriptors.
/// Populated at startup by C# EntityTypeRegistry.RegisterAll() → NativeApi → here.
/// Replaces the traditional .def XML file parsing.
class EntityDefRegistry {
 public:
  static EntityDefRegistry& Instance();

  /// Deserialize a binary entity type descriptor from C# and register it.
  /// Returns true on success.
  bool RegisterType(const std::byte* data, int32_t len);

  /// Load entity definitions from an entity_defs.json file (used by DBApp).
  [[nodiscard]] static auto FromJsonFile(const std::filesystem::path& path)
      -> Result<EntityDefRegistry>;

  /// Find descriptor by type name.
  [[nodiscard]] const EntityTypeDescriptor* FindByName(std::string_view name) const;

  /// Find descriptor by type ID.
  [[nodiscard]] const EntityTypeDescriptor* FindById(uint16_t type_id) const;

  /// Validate that rpc_id belongs to the given entity type.
  [[nodiscard]] bool ValidateRpc(uint16_t type_id, uint32_t rpc_id) const;

  /// Find an RPC descriptor by its packed rpc_id. Returns nullptr if not found.
  [[nodiscard]] const RpcDescriptor* FindRpc(uint32_t rpc_id) const;

  /// Check whether an RPC is exposed to clients.
  [[nodiscard]] bool IsExposed(uint32_t rpc_id) const;

  /// Get the exposed scope for an RPC. Returns None if not found.
  [[nodiscard]] ExposedScope GetExposedScope(uint32_t rpc_id) const;

  /// Get replicated properties for a type, filtered by min_scope.
  /// Returns properties whose scope >= min_scope.
  ///
  /// Current usage (with C# 4-value enum: CellPrivate=0, BaseOnly=1, OwnClient=2, AllClients=3):
  ///   - For owner client: pass OwnClient (2) → selects {OwnClient, AllClients}
  ///   - For other clients: pass OtherClients (3) → selects {AllClients} (C# AllClients == 3)
  ///
  /// WARNING: The C++ ReplicationScope enum has 8 values (OtherClients=3, AllClients=4, etc.)
  /// but C# only sends values 0-3. If .def-based definitions are adopted with the full
  /// 8-value enum, this >= comparison must be replaced with explicit scope matching.
  [[nodiscard]] std::vector<const PropertyDescriptor*> GetReplicatedProperties(
      uint16_t type_id, ReplicationScope min_scope) const;

  /// Get all persistent properties for a type.
  [[nodiscard]] std::vector<const PropertyDescriptor*> GetPersistentProperties(
      uint16_t type_id) const;

  /// Number of registered types.
  [[nodiscard]] size_t TypeCount() const { return types.size(); }

  /// Iterate all registered types (read-only).
  [[nodiscard]] auto AllTypes() const -> const std::vector<EntityTypeDescriptor>& { return types; }

  /// MD5 digest over all persistent property descriptors (type_id, name, data_type, identifier).
  /// Used to verify BaseApp and DBApp have the same entity definitions.
  [[nodiscard]] auto PersistentPropertiesDigest() const -> std::array<uint8_t, 16>;

  /// Clear all registered types (used during hot-reload).
  void clear();

  EntityDefRegistry() = default;

  std::vector<EntityTypeDescriptor> types;
  std::unordered_map<std::string, size_t> name_index;
  std::unordered_map<uint16_t, size_t> id_index;
  // Maps rpc_id → type index for fast RPC validation
  std::unordered_map<uint32_t, size_t> rpc_to_type;
};

}  // namespace atlas

#endif  // ATLAS_LIB_ENTITYDEF_ENTITY_DEF_REGISTRY_H_
