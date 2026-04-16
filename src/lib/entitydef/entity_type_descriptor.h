#ifndef ATLAS_LIB_ENTITYDEF_ENTITY_TYPE_DESCRIPTOR_H_
#define ATLAS_LIB_ENTITYDEF_ENTITY_TYPE_DESCRIPTOR_H_

#include <cstdint>
#include <string>
#include <vector>

namespace atlas {

enum class PropertyDataType : uint8_t {
  kBool,
  kInt8,
  kUInt8,
  kInt16,
  kUInt16,
  kInt32,
  kUInt32,
  kInt64,
  kUInt64,
  kFloat,
  kDouble,
  kString,
  kBytes,
  kVector3,
  kQuaternion,
  kCustom
};

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

/// Exposed scope for RPC methods callable by clients.
enum class ExposedScope : uint8_t {
  kNone = 0,        // Not exposed — server-internal only
  kOwnClient = 1,   // Only the owning client may call
  kAllClients = 2,  // Any client in AoI may call (cell_methods only)
};

struct PropertyDescriptor {
  std::string name;
  PropertyDataType data_type;
  ReplicationScope scope;
  bool persistent{false};
  bool identifier{false};  // [Identifier] — extracted as sm_identifier column in DB
  uint8_t detail_level{5};
  uint16_t index{0};
};

struct RpcDescriptor {
  std::string name;
  uint32_t rpc_id;  // Packed: [direction:2 | typeIndex:14 | method:8]
  std::vector<PropertyDataType> param_types;
  ExposedScope exposed{ExposedScope::kNone};

  [[nodiscard]] uint8_t Direction() const { return static_cast<uint8_t>((rpc_id >> 22) & 0x3); }
  [[nodiscard]] uint16_t TypeIndex() const { return static_cast<uint16_t>((rpc_id >> 8) & 0x3FFF); }
  [[nodiscard]] uint8_t MethodIndex() const { return static_cast<uint8_t>(rpc_id & 0xFF); }
  [[nodiscard]] bool IsExposed() const { return exposed != ExposedScope::kNone; }
};

/// Compression algorithm for entity data on the wire (mirrors network::CompressionType).
enum class EntityCompression : uint8_t {
  kNone = 0,
  kDeflate = 1,
};

struct EntityTypeDescriptor {
  std::string name;
  uint16_t type_id;
  bool has_cell;
  bool has_client;
  std::vector<PropertyDescriptor> properties;
  std::vector<RpcDescriptor> rpcs;

  /// Compression for internal (server-to-server) large messages.
  EntityCompression internal_compression = EntityCompression::kNone;
  /// Compression for external (server-to-client) large messages.
  EntityCompression external_compression = EntityCompression::kNone;
};

}  // namespace atlas

#endif  // ATLAS_LIB_ENTITYDEF_ENTITY_TYPE_DESCRIPTOR_H_
