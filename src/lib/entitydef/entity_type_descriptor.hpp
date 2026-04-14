#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace atlas
{

enum class PropertyDataType : uint8_t
{
    Bool,
    Int8,
    UInt8,
    Int16,
    UInt16,
    Int32,
    UInt32,
    Int64,
    UInt64,
    Float,
    Double,
    String,
    Bytes,
    Vector3,
    Quaternion,
    Custom
};

enum class ReplicationScope : uint8_t
{
    CellPrivate = 0,
    CellPublic = 1,
    OwnClient = 2,
    OtherClients = 3,
    AllClients = 4,
    CellPublicAndOwn = 5,
    Base = 6,
    BaseAndClient = 7,
};

/// Exposed scope for RPC methods callable by clients.
enum class ExposedScope : uint8_t
{
    None = 0,        // Not exposed — server-internal only
    OwnClient = 1,   // Only the owning client may call
    AllClients = 2,  // Any client in AoI may call (cell_methods only)
};

struct PropertyDescriptor
{
    std::string name;
    PropertyDataType data_type;
    ReplicationScope scope;
    bool persistent{false};
    bool identifier{false};  // [Identifier] — extracted as sm_identifier column in DB
    uint8_t detail_level{5};
    uint16_t index{0};
};

struct RpcDescriptor
{
    std::string name;
    uint32_t rpc_id;  // Packed: [direction:2 | typeIndex:14 | method:8]
    std::vector<PropertyDataType> param_types;
    ExposedScope exposed{ExposedScope::None};

    [[nodiscard]] uint8_t direction() const { return static_cast<uint8_t>((rpc_id >> 22) & 0x3); }
    [[nodiscard]] uint16_t type_index() const
    {
        return static_cast<uint16_t>((rpc_id >> 8) & 0x3FFF);
    }
    [[nodiscard]] uint8_t method_index() const { return static_cast<uint8_t>(rpc_id & 0xFF); }
    [[nodiscard]] bool is_exposed() const { return exposed != ExposedScope::None; }
};

/// Compression algorithm for entity data on the wire (mirrors network::CompressionType).
enum class EntityCompression : uint8_t
{
    None = 0,
    Deflate = 1,
};

struct EntityTypeDescriptor
{
    std::string name;
    uint16_t type_id;
    bool has_cell;
    bool has_client;
    std::vector<PropertyDescriptor> properties;
    std::vector<RpcDescriptor> rpcs;

    /// Compression for internal (server-to-server) large messages.
    EntityCompression internal_compression = EntityCompression::None;
    /// Compression for external (server-to-client) large messages.
    EntityCompression external_compression = EntityCompression::None;
};

}  // namespace atlas
