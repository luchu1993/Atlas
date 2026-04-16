#ifndef ATLAS_LIB_DB_SQLITE_SQLITE_DATABASE_H_
#define ATLAS_LIB_DB_SQLITE_SQLITE_DATABASE_H_

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "db/idatabase.h"

namespace atlas {

class SqliteDatabase : public IDatabase {
 public:
  SqliteDatabase() = default;
  ~SqliteDatabase() override;

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

  void MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) override;

  void GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) override;

  void SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) override;

  void LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) override;
  void SaveEntityIdCounter(EntityID next_id, std::function<void(bool success)> callback) override;

  void ProcessResults() override;

  void BeginBatch() override;
  void EndBatch() override;

 private:
  struct EntityRow {
    bool found{false};
    EntityData data;
    std::string password_hash;
    bool auto_load{false};
    std::optional<CheckoutInfo> checked_out_by;
  };

  class Statement {
   public:
    Statement() = default;
    explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
    Statement(const Statement&) = delete;
    auto operator=(const Statement&) -> Statement& = delete;
    Statement(Statement&& other) noexcept;
    auto operator=(Statement&& other) noexcept -> Statement&;
    ~Statement();

    [[nodiscard]] auto Get() const -> sqlite3_stmt* { return stmt_; }
    void Reset();

   private:
    sqlite3_stmt* stmt_{nullptr};
  };

  [[nodiscard]] auto OpenDatabase(const DatabaseConfig& config) -> Result<void>;
  [[nodiscard]] auto EnsureSchema() -> Result<void>;
  [[nodiscard]] auto ExecSql(std::string_view sql) -> Result<void>;
  [[nodiscard]] auto ExecSqlIgnoringDuplicateColumn(std::string_view sql) -> Result<void>;
  [[nodiscard]] auto Prepare(std::string_view sql) -> Result<Statement>;
  [[nodiscard]] auto UpsertMeta(std::string_view key, std::string_view value) -> Result<void>;
  [[nodiscard]] auto FetchByDbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow>;
  [[nodiscard]] auto FetchByName(uint16_t type_id, std::string_view identifier)
      -> Result<EntityRow>;
  [[nodiscard]] auto ReadRow(sqlite3_stmt* stmt) const -> EntityRow;
  [[nodiscard]] auto BindIdentifier(Statement& stmt, int index, const std::string& identifier) const
      -> Result<void>;
  [[nodiscard]] auto BindBlob(Statement& stmt, int index, std::span<const std::byte> blob) const
      -> Result<void>;
  [[nodiscard]] auto SqliteError(std::string_view prefix) const -> Error;
  [[nodiscard]] auto SqliteError(std::string_view prefix, int code) const -> Error;
  void FireOrDefer(std::function<void()> cb);

  // Write-scope helpers: in batch mode these use SAVEPOINTs inside a
  // single outer transaction; outside batch mode they fall back to
  // individual BEGIN IMMEDIATE / COMMIT / ROLLBACK.
  [[nodiscard]] auto BeginWriteScope() -> Result<std::string>;
  void CommitWriteScope(const std::string& scope);
  void RollbackWriteScope(const std::string& scope);

  sqlite3* db_{nullptr};
  const EntityDefRegistry* entity_defs_{nullptr};
  std::filesystem::path db_path_;
  bool started_{false};
  bool deferred_mode_{false};
  std::deque<std::function<void()>> deferred_;

  bool batch_active_{false};
  bool batch_txn_open_{false};
  int batch_savepoint_seq_{0};

  static constexpr int kMaxCallbacksPerTick = 2048;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_SQLITE_SQLITE_DATABASE_H_
