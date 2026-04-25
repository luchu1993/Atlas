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

/// Global registry of entity type descriptors.
/// Populated at startup by C# EntityTypeRegistry.RegisterAll() → NativeApi → here.
/// Replaces the traditional .def XML file parsing.
///
/// Pointer stability: Find*/Resolve* return raw pointers into vectors that
/// are mutated by RegisterType / RegisterStruct / clear(). A subsequent
/// registration can reallocate the underlying storage and leave earlier
/// pointers dangling. Registrations run at process startup; runtime code
/// that caches a lookup across ticks must cache by id / name and re-resolve,
/// not hold the pointer.
class EntityDefRegistry {
 public:
  static EntityDefRegistry& Instance();

  /// Deserialize a binary entity type descriptor from C# and register it.
  /// Returns true on success.
  bool RegisterType(const std::byte* data, int32_t len);

  /// Deserialize a binary struct descriptor and register it.
  /// Blob layout (little-endian):
  ///   [u16 struct_id]
  ///   [PackedInt name_len][name bytes]
  ///   [PackedInt field_count]
  ///   for each field:
  ///     [PackedInt field_name_len][field_name bytes]
  ///     [DataTypeRef]          // recursive, see entity_type_descriptor.h
  ///
  /// Returns false on malformed input, depth overflow, or id collision
  /// with a previously-registered struct of a different name (re-register
  /// of the same (id, name) pair replaces the entry and is tolerated to
  /// support hot-reload — a warning is emitted).
  bool RegisterStruct(const std::byte* data, int32_t len);

  /// Deserialize a binary component descriptor and register it.
  /// Blob layout (little-endian):
  ///   [u16 component_type_id]
  ///   [string name]
  ///   [string base_name]      // empty string when no inheritance
  ///   [u8  locality]          // ComponentLocality
  ///   [PackedInt prop_count]
  ///   for each property:      // identical layout to RegisterType properties
  ///
  /// Same id/name collision rules as RegisterStruct — same (id, name) is a
  /// hot-reload replace; mismatched pair is rejected.
  bool RegisterComponent(const std::byte* data, int32_t len);

  // Container-file format ('ATDF' = Atlas Type Descriptor File). Layout:
  //   [u32 magic = 'ATDF' little-endian]
  //   [u16 version]
  //   [u16 flags]                    reserved (compression / digest / etc.)
  //   [PackedInt struct_count]
  //     for each: [PackedInt blob_len][blob bytes]      → RegisterStruct
  //   [PackedInt component_count]
  //     for each: [PackedInt blob_len][blob bytes]      → RegisterComponent
  //   [PackedInt type_count]
  //     for each: [PackedInt blob_len][blob bytes]      → RegisterType
  // Section order is fixed because types' slot table references
  // component_type_id, and component / entity properties may reference
  // struct_id — both must already be in the registry when the referrer
  // is parsed. Each blob is byte-identical to what the matching
  // Register* method consumes today; the container is pure framing.
  static constexpr uint32_t kBinaryFileMagic = 0x46445441u;  // 'A''T''D''F'
  static constexpr uint16_t kBinaryFileVersion = 1;

  // Counts returned from a successful RegisterFromBinaryFile, useful
  // for caller logging / digest comparison.
  struct LoadedCounts {
    size_t structs{0};
    size_t components{0};
    size_t types{0};
  };

  /// Read a container file emitted by Atlas.Tools.DefDump and dispatch
  /// each record into the matching RegisterStruct / RegisterComponent /
  /// RegisterType. Returns counts on success; on any record failure or
  /// header mismatch returns Error and stops at the first bad record
  /// (caller decides whether to clear() the partial state).
  [[nodiscard]] auto RegisterFromBinaryFile(const std::filesystem::path& path)
      -> Result<LoadedCounts>;

  /// In-memory variant — same wire format, no file I/O. Useful for
  /// tests that craft a buffer directly.
  [[nodiscard]] auto RegisterFromBinaryBuffer(std::span<const std::byte> buf)
      -> Result<LoadedCounts>;

  /// Look up a component descriptor by id. Returns nullptr when unknown.
  [[nodiscard]] const ComponentDescriptor* FindComponentById(uint16_t component_type_id) const;

  /// Look up a component descriptor by name. Returns nullptr when unknown.
  [[nodiscard]] const ComponentDescriptor* FindComponentByName(std::string_view name) const;

  /// Iterate all registered components (read-only).
  [[nodiscard]] auto AllComponents() const -> const std::vector<ComponentDescriptor>& {
    return components;
  }

  /// Number of registered components.
  [[nodiscard]] size_t ComponentCount() const { return components.size(); }

  /// Look up a previously-registered struct by its id. Returns nullptr if
  /// the id is unknown. Matches the naming of FindById / FindByName for
  /// EntityTypeDescriptor lookups.
  [[nodiscard]] const StructDescriptor* FindStructById(uint16_t struct_id) const;

  /// Look up a previously-registered struct by its name. Returns nullptr
  /// if the name is unknown. Case-sensitive.
  [[nodiscard]] const StructDescriptor* FindStructByName(std::string_view name) const;

  /// Iterate all registered structs (read-only).
  [[nodiscard]] auto AllStructs() const -> const std::vector<StructDescriptor>& { return structs; }

  /// Number of registered structs.
  [[nodiscard]] size_t StructCount() const { return structs.size(); }

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

  // Registered struct descriptors (from .def <types> section).
  std::vector<StructDescriptor> structs;
  std::unordered_map<uint16_t, size_t> struct_id_index;
  std::unordered_map<std::string, size_t> struct_name_index;

  // Registered component descriptors. Same pointer-stability caveat as
  // `types` above: cache by id / name and re-resolve, never hold the pointer
  // past a registration.
  std::vector<ComponentDescriptor> components;
  std::unordered_map<uint16_t, size_t> component_id_index;
  std::unordered_map<std::string, size_t> component_name_index;
};

}  // namespace atlas

#endif  // ATLAS_LIB_ENTITYDEF_ENTITY_DEF_REGISTRY_H_
