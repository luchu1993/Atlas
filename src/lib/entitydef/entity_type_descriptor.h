#ifndef ATLAS_LIB_ENTITYDEF_ENTITY_TYPE_DESCRIPTOR_H_
#define ATLAS_LIB_ENTITYDEF_ENTITY_TYPE_DESCRIPTOR_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace atlas {

// Numeric values are the wire encoding; never renumber. kInvalid rejects
// unpopulated DataTypeRef nodes before they can look like kBool.
enum class PropertyDataType : uint8_t {
  kBool = 0,
  kInt8 = 1,
  kUInt8 = 2,
  kInt16 = 3,
  kUInt16 = 4,
  kInt32 = 5,
  kUInt32 = 6,
  kInt64 = 7,
  kUInt64 = 8,
  kFloat = 9,
  kDouble = 10,
  kString = 11,
  kBytes = 12,
  kVector3 = 13,
  kQuaternion = 14,
  kCustom = 15,
  kList = 16,
  kDict = 17,
  kStruct = 18,
  kInvalid = 0xFF,
};

// kDict requires scalar keys. shared_ptr keeps descriptors copyable while the
// tree remains read-only after registration.
struct DataTypeRef {
  PropertyDataType kind = PropertyDataType::kInvalid;
  uint16_t struct_id = 0;
  std::shared_ptr<DataTypeRef> elem;  // kList.elem, kDict.value
  std::shared_ptr<DataTypeRef> key;   // kDict.key
};

// Bounds recursion so a crafted descriptor can't blow the decoder's stack.
inline constexpr std::size_t kMaxDataTypeDepth = 8;

enum class ReplicationScope : uint8_t {
  kCellPrivate = 0,
  kCellPublic = 1,
  kOwnClient = 2,
  kOtherClients = 3,
  kAllClients = 4,
  kCellPublicAndOwn = 5,
  kBase = 6,
  kBaseAndClient = 7,
};

enum class ExposedScope : uint8_t {
  kNone = 0,        // Not exposed; server-internal only
  kOwnClient = 1,   // Only the owning client may call
  kAllClients = 2,  // Any client in AoI may call (cell_methods only)
};

struct PropertyDescriptor {
  std::string name;
  PropertyDataType data_type;
  ReplicationScope scope;
  bool persistent{false};
  bool identifier{false};
  // Retained for schema compatibility; current C# emitters send all property
  // deltas on the reliable channel.
  bool reliable{false};
  uint8_t detail_level{5};
  uint16_t index{0};
  std::optional<DataTypeRef> type_ref;
  // Ignored for scalar properties.
  uint32_t max_size{4096};
};

// Enforced at registration; violation means a generator bug, not user input.
// kCustom is scalar; new user-defined composites must use kStruct.
[[nodiscard]] inline bool ValidatePropertyInvariant(const PropertyDescriptor& prop) {
  const bool is_container = prop.data_type == PropertyDataType::kList ||
                            prop.data_type == PropertyDataType::kDict ||
                            prop.data_type == PropertyDataType::kStruct;
  if (is_container) {
    return prop.type_ref.has_value() && prop.type_ref->kind == prop.data_type;
  }
  return !prop.type_ref.has_value();
}

struct RpcDescriptor {
  std::string name;
  // rpc_id: bit31 reply, bits24-30 slot, bits22-23 dir, bits8-21 type, bits0-7 method.
  // Component RPCs share the owning entity typeIndex; slot 0 is entity body.
  uint32_t rpc_id;
  std::vector<PropertyDataType> param_types;
  ExposedScope exposed{ExposedScope::kNone};

  [[nodiscard]] uint8_t SlotIdx() const { return static_cast<uint8_t>((rpc_id >> 24) & 0x7F); }
  [[nodiscard]] uint8_t Direction() const { return static_cast<uint8_t>((rpc_id >> 22) & 0x3); }
  [[nodiscard]] uint16_t TypeIndex() const { return static_cast<uint16_t>((rpc_id >> 8) & 0x3FFF); }
  [[nodiscard]] uint8_t MethodIndex() const { return static_cast<uint8_t>(rpc_id & 0xFF); }
  [[nodiscard]] bool IsExposed() const { return exposed != ExposedScope::kNone; }
  [[nodiscard]] bool IsComponentRpc() const { return SlotIdx() != 0; }
};

enum class ComponentLocality : uint8_t {
  kSynced = 0,
  kServerLocal = 1,
  kClientLocal = 2,
};

// Reusable component schema keyed by component_type_id; entity slot scope
// further narrows replicated property visibility.
struct ComponentDescriptor {
  std::string name;
  uint16_t component_type_id{0};
  // Empty when the component does not extend another; inheritance is single
  // chain and resolved by name at attach time.
  std::string base_name;
  ComponentLocality locality{ComponentLocality::kSynced};
  // Own properties only; runtime walks base_name when flattening inherited
  // fields so binary descriptors stay compact.
  std::vector<PropertyDescriptor> properties;
};

// Per-entity component attachment metadata. Slot scope is the upper bound;
// tighter property scopes still win and are checked by the C# parser.
struct EntitySlotDescriptor {
  uint16_t slot_idx{0};   // 1-based; slot 0 reserved for entity body.
  std::string slot_name;  // Accessor name on the entity, e.g. "ability".
  uint16_t component_type_id{0};
  ReplicationScope scope{ReplicationScope::kAllClients};
  bool lazy{false};
};

struct EntityTypeDescriptor {
  std::string name;
  uint16_t type_id;
  bool has_cell;
  bool has_client;
  std::vector<PropertyDescriptor> properties;
  std::vector<RpcDescriptor> rpcs;
  std::vector<EntitySlotDescriptor> slots;
};

}  // namespace atlas

#endif  // ATLAS_LIB_ENTITYDEF_ENTITY_TYPE_DESCRIPTOR_H_
