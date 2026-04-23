#ifndef ATLAS_LIB_DB_IDATABASE_H_
#define ATLAS_LIB_DB_IDATABASE_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "entitydef/entity_def_registry.h"
#include "foundation/error.h"
#include "network/address.h"
#include "server/entity_types.h"

namespace atlas {

// ============================================================================
// Type aliases
// ============================================================================

using DatabaseID = int64_t;
inline constexpr DatabaseID kInvalidDBID = 0;

// ============================================================================
// DatabaseConfig
// ============================================================================

struct DatabaseConfig {
  std::string type{"sqlite"};  // "xml", "sqlite", or "mysql"

  // XML backend
  std::filesystem::path xml_dir{"data/db"};

  // SQLite backend
  std::filesystem::path sqlite_path{"data/atlas_dev.sqlite3"};
  bool sqlite_wal{true};
  int sqlite_busy_timeout_ms{5000};
  bool sqlite_foreign_keys{true};

  // MySQL backend
  std::string mysql_host{"127.0.0.1"};
  uint16_t mysql_port{3306};
  std::string mysql_user;
  std::string mysql_password;
  std::string mysql_database{"atlas"};
  int mysql_pool_size{4};
};

// ============================================================================
// WriteFlags
// ============================================================================

enum class WriteFlags : uint8_t {
  kNone = 0,
  kCreateNew = 1 << 0,     // new entity, dbid=0, DB assigns ID
  kExplicitDbid = 1 << 1,  // use specified dbid
  kLogOff = 1 << 2,        // entity offline, clear checkout record
  kDelete = 1 << 3,        // delete from database
  kAutoLoadOn = 1 << 4,    // mark as auto-load
  kAutoLoadOff = 1 << 5,   // clear auto-load
  kCellData = 1 << 6,      // reserved: blob includes cell data
};

[[nodiscard]] inline auto operator|(WriteFlags a, WriteFlags b) -> WriteFlags {
  return static_cast<WriteFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

[[nodiscard]] inline auto operator&(WriteFlags a, WriteFlags b) -> WriteFlags {
  return static_cast<WriteFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
}

[[nodiscard]] inline auto HasFlag(WriteFlags flags, WriteFlags f) -> bool {
  return (static_cast<uint8_t>(flags) & static_cast<uint8_t>(f)) != 0;
}

// ============================================================================
// Data structures
// ============================================================================

struct EntityData {
  DatabaseID dbid{kInvalidDBID};
  uint16_t type_id{0};
  std::vector<std::byte> blob;  // persistent attribute binary (C# SpanWriter output)
  std::string identifier;       // value of [Identifier] property (if any)
};

struct CheckoutInfo {
  Address base_addr;      // owning BaseApp internal address
  uint32_t app_id{0};     // owning BaseApp AppID (more stable than address)
  uint32_t entity_id{0};  // EntityID on the owning BaseApp
};

// ============================================================================
// Async operation result types
// ============================================================================

struct PutResult {
  bool success{false};
  DatabaseID dbid{kInvalidDBID};  // assigned or existing DBID
  std::string error;
};

struct GetResult {
  bool success{false};
  EntityData data;
  std::optional<CheckoutInfo> checked_out_by;  // non-empty if already checked out
  std::string error;
};

struct DelResult {
  bool success{false};
  std::string error;
};

struct LookupResult {
  bool found{false};
  DatabaseID dbid{kInvalidDBID};
  std::string password_hash;  // sm_passwordHash (may be empty)
  std::string error;          // non-empty on backend failure (distinct from not-found)
};

// ============================================================================
// IDatabase — database abstraction interface
// ============================================================================

class IDatabase {
 public:
  virtual ~IDatabase() = default;

  // Non-copyable
  IDatabase(const IDatabase&) = delete;
  IDatabase& operator=(const IDatabase&) = delete;

  /// Initialize: connect, create/migrate schema.
  [[nodiscard]] virtual auto Startup(const DatabaseConfig& config,
                                     const EntityDefRegistry& entity_defs) -> Result<void> = 0;

  /// Shutdown cleanly.
  virtual void Shutdown() = 0;

  // =====================================================================
  // Entity CRUD (async)
  // =====================================================================

  /// Save entity (create or update).
  /// dbid=0 (kInvalidDBID) means new entity — DB assigns ID.
  virtual void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                         std::span<const std::byte> blob, const std::string& identifier,
                         std::function<void(PutResult)> callback) = 0;

  /// Save entity with an explicit password hash payload.
  /// Used by DBApp account creation so AuthLogin can validate credentials.
  virtual void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                     std::span<const std::byte> blob, const std::string& identifier,
                                     const std::string& /*password_hash*/,
                                     std::function<void(PutResult)> callback) {
    PutEntity(dbid, type_id, flags, blob, identifier, std::move(callback));
  }

  /// Load entity by DBID.
  virtual void GetEntity(DatabaseID dbid, uint16_t type_id,
                         std::function<void(GetResult)> callback) = 0;

  /// Delete entity.
  virtual void DelEntity(DatabaseID dbid, uint16_t type_id,
                         std::function<void(DelResult)> callback) = 0;

  /// Find DBID by identifier (name).
  virtual void LookupByName(uint16_t type_id, const std::string& identifier,
                            std::function<void(LookupResult)> callback) = 0;

  // =====================================================================
  // Checkout tracking
  // =====================================================================

  /// Atomic checkout: SELECT + mark checked-out in a single transaction.
  /// If already checked out, GetResult::checked_out_by is non-empty.
  virtual void CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                              std::function<void(GetResult)> callback) = 0;

  /// Atomic checkout by identifier name.
  virtual void CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                    const CheckoutInfo& new_owner,
                                    std::function<void(GetResult)> callback) = 0;

  /// Clear checkout for a single entity (e.g. logoff without blob update).
  virtual void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                             std::function<void(bool)> callback) = 0;

  /// Batch-clear all checkouts held by a dead BaseApp.
  virtual void ClearCheckoutsForAddress(const Address& base_addr,
                                        std::function<void(int cleared_count)> callback) = 0;

  /// Fire-and-forget checkout clear (no callback, no flush).
  /// Used by the optimistic-checkin fast path to avoid scheduling overhead.
  virtual void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) {
    ClearCheckout(dbid, type_id, [](bool) {});
  }

  // =====================================================================
  // Auto-load
  // =====================================================================

  /// Return all entities marked for auto-load (called at startup).
  virtual void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) = 0;

  /// Set or clear the auto-load flag for an entity.
  virtual void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) = 0;

  // =====================================================================
  // EntityID counter persistence
  // =====================================================================

  /// Load the next EntityID counter from persistent storage.
  /// Called at startup to recover the allocator state.
  /// If no counter exists yet, callback receives 1 (first valid ID).
  virtual void LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) = 0;

  /// Persist the next EntityID counter so it survives restarts.
  virtual void SaveEntityIdCounter(EntityID next_id,
                                   std::function<void(bool success)> callback) = 0;

  // =====================================================================
  // Main-thread pump
  // =====================================================================

  /// Enable deferred-callback mode: queue callbacks and invoke them only
  /// from process_results(), giving the event loop room to process ACKs.
  virtual void SetDeferredMode(bool /*enabled*/) {}

  /// Collect completed async callbacks and invoke them on the calling (main) thread.
  /// Call from DBApp::on_tick_complete().
  virtual void ProcessResults() = 0;

  // =====================================================================
  // Write batching
  // =====================================================================

  /// Begin a batch: all subsequent write operations share a single outer
  /// transaction until end_batch() is called.  Individual operations use
  /// SAVEPOINTs for per-operation rollback.  No-op if unsupported.
  virtual void BeginBatch() {}

  /// Commit the batched transaction (if any writes occurred) and leave
  /// batch mode.  No-op if unsupported or if no batch is active.
  virtual void EndBatch() {}

  /// Whether this backend supports multiple simultaneous DBApp instances.
  [[nodiscard]] virtual auto SupportsMultiDbapp() const -> bool { return false; }

 protected:
  IDatabase() = default;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_IDATABASE_H_
