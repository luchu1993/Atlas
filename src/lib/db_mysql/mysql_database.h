#ifndef ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_
#define ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "db/idatabase.h"

struct st_mysql;

namespace atlas {

// MySQL backend. In deferred mode (how DBApp drives it) operations run on a
// pool of worker threads, each with its own connection, keyed by entity so a
// given entity's ops stay ordered; results are delivered on the main thread via
// ProcessResults, so DB round-trips never block the tick loop. In non-deferred
// mode it runs inline on a bootstrap connection for simple/test callers.
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

  [[nodiscard]] auto SupportsMultiDbapp() const -> bool override { return true; }

 private:
  struct EntityRow {
    bool found{false};
    EntityData data;
    std::string password_hash;
    bool auto_load{false};
    std::optional<CheckoutInfo> checked_out_by;
  };

  struct Worker {
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void(st_mysql*)>> jobs;
    bool stop{false};
  };

  // Runs op on a connection and routes its result: inline on the bootstrap
  // connection when synchronous, else onto the entity's worker with the result
  // delivered through ProcessResults.
  template <typename R>
  void Dispatch(uint16_t type_id, uint64_t key, std::function<R(st_mysql*)> op,
                std::function<void(R)> callback) {
    if (!deferred_mode_) {
      callback(op(conn_));
      return;
    }
    EnqueueForEntity(
        type_id, key, [this, op = std::move(op), cb = std::move(callback)](st_mysql* conn) mutable {
          R r = op(conn);
          PostResult([cb = std::move(cb), r = std::move(r)]() mutable { cb(std::move(r)); });
        });
  }

  // Result-less variant for fire-and-forget setters (SetAutoLoad).
  void DispatchVoid(uint16_t type_id, uint64_t key, std::function<void(st_mysql*)> op) {
    if (!deferred_mode_) {
      op(conn_);
      return;
    }
    EnqueueForEntity(type_id, key, std::move(op));
  }

  void EnqueueForEntity(uint16_t type_id, uint64_t key, std::function<void(st_mysql*)> job);
  void PostResult(std::function<void()> result);
  void WorkerLoop(Worker& worker);
  [[nodiscard]] auto OpenConnection() const -> st_mysql*;

  static auto EnsureSchema(st_mysql* conn) -> Result<void>;
  static auto ExecSql(st_mysql* conn, std::string_view sql) -> Result<void>;
  static auto QueryRow(st_mysql* conn, std::string_view sql) -> Result<EntityRow>;
  static auto ReadRow(char** row, const unsigned long* lengths) -> EntityRow;
  static auto FetchByDbid(st_mysql* conn, DatabaseID dbid, uint16_t type_id) -> Result<EntityRow>;
  static auto FetchByName(st_mysql* conn, uint16_t type_id, std::string_view identifier)
      -> Result<EntityRow>;
  static auto MysqlErr(st_mysql* conn, std::string_view prefix) -> Error;
  static auto ConstraintErrorKind(st_mysql* conn) -> DatabaseErrorKind;
  // Binary/identifier values go into SQL as X'..' hex literals: byte-exact and
  // injection-proof without escaping. Empty identifier maps to NULL.
  static auto HexLiteral(std::span<const std::byte> data) -> std::string;
  static auto IdentifierLiteral(std::string_view identifier) -> std::string;
  static auto BeginTxn(st_mysql* conn) -> Result<void>;
  static void Commit(st_mysql* conn);
  static void Rollback(st_mysql* conn);

  // Operation bodies: pure SQL against a connection, returning the result. The
  // public methods wrap these in Dispatch. write_password gates the password
  // column so PutEntity leaves it untouched.
  static auto DoPut(st_mysql* conn, DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                    std::span<const std::byte> blob, const std::string& identifier,
                    bool write_password, const std::string& password_hash) -> PutResult;
  static auto DoGet(st_mysql* conn, DatabaseID dbid, uint16_t type_id) -> GetResult;
  static auto DoDel(st_mysql* conn, DatabaseID dbid, uint16_t type_id) -> DelResult;
  static auto DoLookup(st_mysql* conn, uint16_t type_id, const std::string& identifier)
      -> LookupResult;
  static auto DoCheckout(st_mysql* conn, DatabaseID dbid, uint16_t type_id,
                         const CheckoutInfo& new_owner) -> GetResult;
  static auto DoCheckoutByName(st_mysql* conn, uint16_t type_id, const std::string& identifier,
                               const CheckoutInfo& new_owner) -> GetResult;
  static auto CheckoutTail(st_mysql* conn, DatabaseID dbid, uint16_t type_id, EntityRow row,
                           const CheckoutInfo& new_owner, int64_t now_ms) -> GetResult;
  static auto DoClearCheckout(st_mysql* conn, DatabaseID dbid, uint16_t type_id) -> bool;
  static auto DoClearForAddress(st_mysql* conn, const Address& base_addr) -> int;
  static auto DoGetAutoLoad(st_mysql* conn) -> std::vector<EntityData>;
  static void DoSetAutoLoad(st_mysql* conn, DatabaseID dbid, uint16_t type_id, bool auto_load);
  static auto DoGetMaxDbid(st_mysql* conn, DatabaseID low, DatabaseID high) -> DbidRangeResult;
  static auto DoLoadCounter(st_mysql* conn) -> EntityID;
  static auto DoSaveCounter(st_mysql* conn, EntityID next_id) -> bool;

  st_mysql* conn_{nullptr};  // bootstrap + inline (non-deferred) connection
  const EntityDefRegistry* entity_defs_{nullptr};
  DatabaseConfig config_;
  bool started_{false};
  bool deferred_mode_{false};

  std::vector<std::unique_ptr<Worker>> workers_;

  std::mutex results_mutex_;
  std::deque<std::function<void()>> results_;

  static constexpr int kMaxCallbacksPerTick = 2048;
};

}  // namespace atlas

#endif  // ATLAS_LIB_DB_MYSQL_MYSQL_DATABASE_H_
