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
#include "entitydef/struct_descriptor.h"
#include "foundation/error.h"

namespace atlas {

// Lookup results point into vectors mutated by registration; cache ids or names,
// not returned pointers, across registry updates.
class EntityDefRegistry {
 public:
  static EntityDefRegistry& Instance();

  bool RegisterType(const std::byte* data, int32_t len);

  bool RegisterStruct(const std::byte* data, int32_t len);

  bool RegisterComponent(const std::byte* data, int32_t len);

  // Section order is fixed because types' slot table references
  // component_type_id, and component / entity properties may reference
  // struct_id; both must already be registered before the referrer is parsed.
  static constexpr uint32_t kBinaryFileMagic = 0x46445441u;  // 'A''T''D''F'
  static constexpr uint16_t kBinaryFileVersion = 1;

  struct LoadedCounts {
    size_t structs{0};
    size_t components{0};
    size_t types{0};
  };

  [[nodiscard]] auto RegisterFromBinaryFile(const std::filesystem::path& path)
      -> Result<LoadedCounts>;

  [[nodiscard]] auto RegisterFromBinaryBuffer(std::span<const std::byte> buf)
      -> Result<LoadedCounts>;

  [[nodiscard]] const ComponentDescriptor* FindComponentById(uint16_t component_type_id) const;

  [[nodiscard]] const ComponentDescriptor* FindComponentByName(std::string_view name) const;

  [[nodiscard]] auto AllComponents() const -> const std::vector<ComponentDescriptor>& {
    return components;
  }

  [[nodiscard]] size_t ComponentCount() const { return components.size(); }

  [[nodiscard]] const StructDescriptor* FindStructById(uint16_t struct_id) const;

  [[nodiscard]] const StructDescriptor* FindStructByName(std::string_view name) const;

  [[nodiscard]] auto AllStructs() const -> const std::vector<StructDescriptor>& { return structs; }

  [[nodiscard]] size_t StructCount() const { return structs.size(); }

  [[nodiscard]] const EntityTypeDescriptor* FindByName(std::string_view name) const;

  [[nodiscard]] const EntityTypeDescriptor* FindById(uint16_t type_id) const;

  [[nodiscard]] bool ValidateRpc(uint16_t type_id, uint32_t rpc_id) const;

  [[nodiscard]] const RpcDescriptor* FindRpc(uint32_t rpc_id) const;

  [[nodiscard]] bool IsExposed(uint32_t rpc_id) const;

  [[nodiscard]] ExposedScope GetExposedScope(uint32_t rpc_id) const;

  // C# currently emits scope values 0-3; widening the enum needs explicit matching here.
  [[nodiscard]] std::vector<const PropertyDescriptor*> GetReplicatedProperties(
      uint16_t type_id, ReplicationScope min_scope) const;

  [[nodiscard]] std::vector<const PropertyDescriptor*> GetPersistentProperties(
      uint16_t type_id) const;

  [[nodiscard]] size_t TypeCount() const { return types.size(); }

  [[nodiscard]] auto AllTypes() const -> const std::vector<EntityTypeDescriptor>& { return types; }

  [[nodiscard]] auto PersistentPropertiesDigest() const -> std::array<uint8_t, 16>;

  void clear();

  EntityDefRegistry() = default;

  std::vector<EntityTypeDescriptor> types;
  std::unordered_map<std::string, size_t> name_index;
  std::unordered_map<uint16_t, size_t> id_index;
  std::unordered_map<uint32_t, size_t> rpc_to_type;

  std::vector<StructDescriptor> structs;
  std::unordered_map<uint16_t, size_t> struct_id_index;
  std::unordered_map<std::string, size_t> struct_name_index;

  std::vector<ComponentDescriptor> components;
  std::unordered_map<uint16_t, size_t> component_id_index;
  std::unordered_map<std::string, size_t> component_name_index;
};

}  // namespace atlas

#endif  // ATLAS_LIB_ENTITYDEF_ENTITY_DEF_REGISTRY_H_
