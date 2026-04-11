#pragma once

#include "entitydef/entity_def_registry.hpp"
#include "foundation/error.hpp"
#include "network/address.hpp"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace atlas
{

// ============================================================================
// Type aliases
// ============================================================================

using DatabaseID = int64_t;
inline constexpr DatabaseID kInvalidDBID = 0;

// ============================================================================
// DatabaseConfig
// ============================================================================

struct DatabaseConfig
{
    std::string type;  // "xml" or "mysql"

    // XML backend
    std::filesystem::path xml_dir{"data/db"};

    // MySQL backend
    std::string mysql_host{"127.0.0.1"};
    uint16_t mysql_port{3306};
    std::string mysql_user;
    std::string mysql_password;
    std::string mysql_database{"atlas"};
    int mysql_pool_size{4};
};

// ============================================================================
// WriteFlags — mirrors BigWorld WriteDBFlags
// ============================================================================

enum class WriteFlags : uint8_t
{
    None = 0,
    CreateNew = 1 << 0,     // new entity, dbid=0, DB assigns ID
    ExplicitDBID = 1 << 1,  // use specified dbid
    LogOff = 1 << 2,        // entity offline, clear checkout record
    Delete = 1 << 3,        // delete from database
    AutoLoadOn = 1 << 4,    // mark as auto-load
    AutoLoadOff = 1 << 5,   // clear auto-load
    CellData = 1 << 6,      // reserved: blob includes cell data (Phase 10)
};

[[nodiscard]] inline auto operator|(WriteFlags a, WriteFlags b) -> WriteFlags
{
    return static_cast<WriteFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] inline auto operator&(WriteFlags a, WriteFlags b) -> WriteFlags
{
    return static_cast<WriteFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

[[nodiscard]] inline auto has_flag(WriteFlags flags, WriteFlags f) -> bool
{
    return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(f)) != 0;
}

// ============================================================================
// Data structures
// ============================================================================

struct EntityData
{
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    std::vector<std::byte> blob;  // persistent attribute binary (C# SpanWriter output)
    std::string identifier;       // value of [Identifier] property (if any)
};

struct CheckoutInfo
{
    Address base_addr;      // owning BaseApp internal address
    uint32_t app_id{0};     // owning BaseApp AppID (more stable than address)
    uint32_t entity_id{0};  // EntityID on the owning BaseApp
};

// ============================================================================
// Async operation result types
// ============================================================================

struct PutResult
{
    bool success{false};
    DatabaseID dbid{kInvalidDBID};  // assigned or existing DBID
    std::string error;
};

struct GetResult
{
    bool success{false};
    EntityData data;
    std::optional<CheckoutInfo> checked_out_by;  // non-empty if already checked out
    std::string error;
};

struct DelResult
{
    bool success{false};
    std::string error;
};

struct LookupResult
{
    bool found{false};
    DatabaseID dbid{kInvalidDBID};
    std::string password_hash;  // sm_passwordHash (may be empty)
};

// ============================================================================
// IDatabase — database abstraction interface
// ============================================================================

class IDatabase
{
public:
    virtual ~IDatabase() = default;

    // Non-copyable
    IDatabase(const IDatabase&) = delete;
    IDatabase& operator=(const IDatabase&) = delete;

    /// Initialize: connect, create/migrate schema.
    [[nodiscard]] virtual auto startup(const DatabaseConfig& config,
                                       const EntityDefRegistry& entity_defs) -> Result<void> = 0;

    /// Shutdown cleanly.
    virtual void shutdown() = 0;

    // =====================================================================
    // Entity CRUD (async)
    // =====================================================================

    /// Save entity (create or update).
    /// dbid=0 (kInvalidDBID) means new entity — DB assigns ID.
    virtual void put_entity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                            std::span<const std::byte> blob, const std::string& identifier,
                            std::function<void(PutResult)> callback) = 0;

    /// Load entity by DBID.
    virtual void get_entity(DatabaseID dbid, uint16_t type_id,
                            std::function<void(GetResult)> callback) = 0;

    /// Delete entity.
    virtual void del_entity(DatabaseID dbid, uint16_t type_id,
                            std::function<void(DelResult)> callback) = 0;

    /// Find DBID by identifier (name).
    virtual void lookup_by_name(uint16_t type_id, const std::string& identifier,
                                std::function<void(LookupResult)> callback) = 0;

    // =====================================================================
    // Checkout tracking
    // =====================================================================

    /// Atomic checkout: SELECT + mark checked-out in a single transaction.
    /// If already checked out, GetResult::checked_out_by is non-empty.
    virtual void checkout_entity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                                 std::function<void(GetResult)> callback) = 0;

    /// Atomic checkout by identifier name.
    virtual void checkout_entity_by_name(uint16_t type_id, const std::string& identifier,
                                         const CheckoutInfo& new_owner,
                                         std::function<void(GetResult)> callback) = 0;

    /// Clear checkout for a single entity (e.g. logoff without blob update).
    virtual void clear_checkout(DatabaseID dbid, uint16_t type_id,
                                std::function<void(bool)> callback) = 0;

    /// Batch-clear all checkouts held by a dead BaseApp.
    virtual void clear_checkouts_for_address(const Address& base_addr,
                                             std::function<void(int cleared_count)> callback) = 0;

    // =====================================================================
    // Auto-load
    // =====================================================================

    /// Return all entities marked for auto-load (called at startup).
    virtual void get_auto_load_entities(std::function<void(std::vector<EntityData>)> callback) = 0;

    /// Set or clear the auto-load flag for an entity.
    virtual void set_auto_load(DatabaseID dbid, uint16_t type_id, bool auto_load) = 0;

    // =====================================================================
    // Main-thread pump
    // =====================================================================

    /// Collect completed async callbacks and invoke them on the calling (main) thread.
    /// Call from DBApp::on_tick_complete().
    virtual void process_results() = 0;

    /// Whether this backend supports multiple simultaneous DBApp instances.
    [[nodiscard]] virtual auto supports_multi_dbapp() const -> bool { return false; }

protected:
    IDatabase() = default;
};

}  // namespace atlas
