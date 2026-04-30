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

using DatabaseID = int64_t;
inline constexpr DatabaseID kInvalidDBID = 0;

struct DatabaseConfig {
  std::string type{"sqlite"};  // "xml", "sqlite", or "mysql"

  std::filesystem::path xml_dir{"data/db"};

  std::filesystem::path sqlite_path{"data/atlas_dev.sqlite3"};
  bool sqlite_wal{true};
  int sqlite_busy_timeout_ms{5000};
  bool sqlite_foreign_keys{true};

  std::string mysql_host{"127.0.0.1"};
  uint16_t mysql_port{3306};
  std::string mysql_user;
  std::string mysql_password;
  std::string mysql_database{"atlas"};
  int mysql_pool_size{4};
};

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

class IDatabase {
 public:
  virtual ~IDatabase() = default;

  IDatabase(const IDatabase&) = delete;
  IDatabase& operator=(const IDatabase&) = delete;

  [[nodiscard]] virtual auto Startup(const DatabaseConfig& config,
                                     const EntityDefRegistry& entity_defs) -> Result<void> = 0;

  virtual void Shutdown() = 0;

  virtual void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                         std::span<const std::byte> blob, const std::string& identifier,
                         std::function<void(PutResult)> callback) = 0;

  virtual void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                     std::span<const std::byte> blob, const std::string& identifier,
                                     const std::string& /*password_hash*/,
                                     std::function<void(PutResult)> callback) {
    PutEntity(dbid, type_id, flags, blob, identifier, std::move(callback));
  }

  virtual void GetEntity(DatabaseID dbid, uint16_t type_id,
                         std::function<void(GetResult)> callback) = 0;

  virtual void DelEntity(DatabaseID dbid, uint16_t type_id,
                         std::function<void(DelResult)> callback) = 0;

  virtual void LookupByName(uint16_t type_id, const std::string& identifier,
                            std::function<void(LookupResult)> callback) = 0;

  virtual void CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                              std::function<void(GetResult)> callback) = 0;

  virtual void CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                    const CheckoutInfo& new_owner,
                                    std::function<void(GetResult)> callback) = 0;

  virtual void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                             std::function<void(bool)> callback) = 0;

  virtual void ClearCheckoutsForAddress(const Address& base_addr,
                                        std::function<void(int cleared_count)> callback) = 0;

  virtual void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) {
    ClearCheckout(dbid, type_id, [](bool) {});
  }

  virtual void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) = 0;

  virtual void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) = 0;

  virtual void LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) = 0;

  virtual void SaveEntityIdCounter(EntityID next_id,
                                   std::function<void(bool success)> callback) = 0;

  virtual void SetDeferredMode(bool /*enabled*/) {}

  virtual void ProcessResults() = 0;

  virtual void BeginBatch() {}

  virtual void EndBatch() {}

  [[nodiscard]] virtual auto SupportsMultiDbapp() const -> bool { return false; }

 protected:
  IDatabase() = default;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_IDATABASE_H_
