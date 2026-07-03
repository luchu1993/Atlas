#ifndef ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_
#define ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_

#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "db/idatabase.h"

struct st_mysql;

namespace atlas {

// P7.4.1 scaffold: Startup connects and provisions the schema on the calling
// thread; CRUD/checkout land in P7.4.2+. A worker pool replaces this inline
// connection in P7.4.4, at which point callbacks arrive off-thread and are
// delivered on the main thread through ProcessResults.
class MysqlDatabase : public IDatabase {
 public:
  MysqlDatabase() = default;
  ~MysqlDatabase() override;

  void SetDeferredMode(bool enabled) override { deferred_mode_ = enabled; }

  [[nodiscard]] auto Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
      -> Result<void> override;
  void Shutdown() override;

  void PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                 std::span<const std::byte> blob, const std::string& identifier,
                 std::function<void(PutResult)> callback) override;

  void PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                             std::span<const std::byte> blob, const std::string& identifier,
                             const std::string& password_hash,
                             std::function<void(PutResult)> callback) override;

  void GetEntity(DatabaseID dbid, uint16_t type_id,
                 std::function<void(GetResult)> callback) override;

  void DelEntity(DatabaseID dbid, uint16_t type_id,
                 std::function<void(DelResult)> callback) override;

  void LookupByName(uint16_t type_id, const std::string& identifier,
                    std::function<void(LookupResult)> callback) override;

  void CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                      std::function<void(GetResult)> callback) override;

  void CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                            const CheckoutInfo& new_owner,
                            std::function<void(GetResult)> callback) override;

  void ClearCheckout(DatabaseID dbid, uint16_t type_id,
                     std::function<void(bool)> callback) override;

  void ClearCheckoutsForAddress(const Address& base_addr,
                                std::function<void(int cleared_count)> callback) override;

  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) override;

  void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) override;

  void GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                         std::function<void(DbidRangeResult)> callback) override;

  void LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) override;
  void SaveEntityIdCounter(EntityID next_id, std::function<void(bool success)> callback) override;

  void ProcessResults() override;

 private:
  struct EntityRow {
    bool found{false};
    EntityData data;
    std::string password_hash;
    bool auto_load{false};
    std::optional<CheckoutInfo> checked_out_by;
  };

  [[nodiscard]] auto Connect(const DatabaseConfig& config) -> Result<void>;
  [[nodiscard]] auto EnsureSchema() -> Result<void>;
  [[nodiscard]] auto ExecSql(std::string_view sql) -> Result<void>;
  [[nodiscard]] auto FetchByDbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow>;
  [[nodiscard]] auto FetchByName(uint16_t type_id, std::string_view identifier)
      -> Result<EntityRow>;
  [[nodiscard]] auto QueryRow(std::string_view sql) -> Result<EntityRow>;
  // row is a MYSQL_ROW (char**); lengths a MYSQL result length array. Typed as
  // char**/unsigned long* so the header stays free of the connector include.
  [[nodiscard]] auto ReadRow(char** row, const unsigned long* lengths) const -> EntityRow;
  [[nodiscard]] auto MysqlError(std::string_view prefix) const -> Error;
  [[nodiscard]] auto ConstraintErrorKind() const -> DatabaseErrorKind;
  // Binary/identifier values go into SQL as X'..' hex literals: byte-exact and
  // injection-proof without escaping. Empty identifier maps to NULL.
  [[nodiscard]] static auto HexLiteral(std::span<const std::byte> data) -> std::string;
  [[nodiscard]] static auto IdentifierLiteral(std::string_view identifier) -> std::string;
  // Shared insert/update path; write_password gates the password_hash column so
  // PutEntity leaves it untouched while PutEntityWithPassword sets it.
  void PutInternal(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                   std::span<const std::byte> blob, const std::string& identifier,
                   bool write_password, const std::string& password_hash,
                   std::function<void(PutResult)> callback);
  void FireOrDefer(std::function<void()> cb);

  st_mysql* conn_{nullptr};
  const EntityDefRegistry* entity_defs_{nullptr};
  bool started_{false};
  bool deferred_mode_{false};
  std::deque<std::function<void()>> deferred_;

  static constexpr int kMaxCallbacksPerTick = 2048;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_
