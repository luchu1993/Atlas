#include "db_mysql/mysql_database.h"

#include <mysql.h>

#include <cstdint>
#include <format>

#include "foundation/log.h"

namespace atlas {

namespace {
constexpr int kMysqlSchemaVersion = 1;
}  // namespace

MysqlDatabase::~MysqlDatabase() {
  if (started_) {
    Shutdown();
  }
}

auto MysqlDatabase::Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void> {
  entity_defs_ = &entity_defs;

  auto connect_result = Connect(config);
  if (!connect_result) {
    return connect_result.Error();
  }

  auto schema_result = EnsureSchema();
  if (!schema_result) {
    Shutdown();
    return schema_result.Error();
  }

  started_ = true;
  ATLAS_LOG_INFO("MysqlDatabase: connected to {}:{}/{}", config.mysql_host, config.mysql_port,
                 config.mysql_database);
  return {};
}

void MysqlDatabase::Shutdown() {
  if (conn_ != nullptr) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
  deferred_.clear();
  started_ = false;
}

auto MysqlDatabase::Connect(const DatabaseConfig& config) -> Result<void> {
  conn_ = mysql_init(nullptr);
  if (conn_ == nullptr) {
    return Error{ErrorCode::kIoError, "MysqlDatabase: mysql_init failed (out of memory)"};
  }

  unsigned int connect_timeout = 10;
  mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
  mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");

  auto* ok = mysql_real_connect(conn_, config.mysql_host.c_str(), config.mysql_user.c_str(),
                                config.mysql_password.c_str(), config.mysql_database.c_str(),
                                config.mysql_port, nullptr, 0);
  if (ok == nullptr) {
    auto err = MysqlError("MysqlDatabase: mysql_real_connect failed");
    mysql_close(conn_);
    conn_ = nullptr;
    return err;
  }
  return {};
}

auto MysqlDatabase::EnsureSchema() -> Result<void> {
  // Indexes live inside CREATE TABLE so the whole schema is idempotent through
  // IF NOT EXISTS; MySQL has no CREATE INDEX IF NOT EXISTS. Columns are binary
  // so identifier comparison stays byte-exact, matching the SQLite backend.
  auto entities = ExecSql(
      "CREATE TABLE IF NOT EXISTS entities ("
      "dbid BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,"
      "type_id INT UNSIGNED NOT NULL,"
      "`blob` LONGBLOB NOT NULL,"
      "identifier VARBINARY(255) NULL,"
      "password_hash VARBINARY(255) NULL,"
      "auto_load TINYINT NOT NULL DEFAULT 0,"
      "checked_out TINYINT NOT NULL DEFAULT 0,"
      "checkout_ip INT UNSIGNED NOT NULL DEFAULT 0,"
      "checkout_port INT UNSIGNED NOT NULL DEFAULT 0,"
      "checkout_app_id INT UNSIGNED NOT NULL DEFAULT 0,"
      "checkout_eid INT UNSIGNED NOT NULL DEFAULT 0,"
      "created_at_ms BIGINT NOT NULL DEFAULT 0,"
      "updated_at_ms BIGINT NOT NULL DEFAULT 0,"
      "PRIMARY KEY (dbid),"
      "UNIQUE KEY uk_entities_type_identifier (type_id, identifier),"
      "KEY idx_entities_type_dbid (type_id, dbid),"
      "KEY idx_entities_auto_load (auto_load, type_id, dbid),"
      "KEY idx_entities_checkout_addr (checked_out, checkout_ip, checkout_port)"
      ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!entities) return entities.Error();

  auto meta = ExecSql(
      "CREATE TABLE IF NOT EXISTS meta ("
      "`key` VARBINARY(64) NOT NULL,"
      "`value` VARBINARY(255) NOT NULL,"
      "PRIMARY KEY (`key`)"
      ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!meta) return meta.Error();

  auto version =
      ExecSql(std::format("INSERT INTO meta(`key`,`value`) VALUES('schema_version','{}') "
                          "ON DUPLICATE KEY UPDATE `value`=VALUES(`value`)",
                          kMysqlSchemaVersion));
  if (!version) return version.Error();

  auto counter = ExecSql(
      "CREATE TABLE IF NOT EXISTS atlas_entity_id_counter ("
      "id INT NOT NULL,"
      "next_id BIGINT UNSIGNED NOT NULL DEFAULT 1,"
      "PRIMARY KEY (id),"
      "CHECK (id = 1)"
      ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!counter) return counter.Error();

  auto seed = ExecSql("INSERT IGNORE INTO atlas_entity_id_counter(id, next_id) VALUES(1, 1)");
  if (!seed) return seed.Error();

  return {};
}

auto MysqlDatabase::ExecSql(std::string_view sql) -> Result<void> {
  if (mysql_real_query(conn_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return MysqlError(std::format("MysqlDatabase: query failed [{}]", sql));
  }
  // DDL/INSERT return no rows; free any result to keep the connection clean.
  MYSQL_RES* res = mysql_store_result(conn_);
  if (res != nullptr) {
    mysql_free_result(res);
  }
  return {};
}

auto MysqlDatabase::MysqlError(std::string_view prefix) const -> Error {
  const char* msg = (conn_ != nullptr) ? mysql_error(conn_) : "no connection";
  return Error{ErrorCode::kIoError, std::format("{}: {}", prefix, msg)};
}

void MysqlDatabase::FireOrDefer(std::function<void()> cb) {
  if (deferred_mode_) {
    deferred_.push_back(std::move(cb));
  } else {
    cb();
  }
}

void MysqlDatabase::ProcessResults() {
  int fired = 0;
  while (!deferred_.empty() && fired < kMaxCallbacksPerTick) {
    auto cb = std::move(deferred_.front());
    deferred_.pop_front();
    cb();
    ++fired;
  }
}

// ── P7.4.1 stubs: query paths land in P7.4.2+ ───────────────────────────────
namespace {
constexpr std::string_view kNotImpl = "MysqlDatabase: query path not implemented (P7.4.1 scaffold)";
}  // namespace

void MysqlDatabase::PutEntity(DatabaseID, uint16_t, WriteFlags, std::span<const std::byte>,
                              const std::string&, std::function<void(PutResult)> callback) {
  PutResult result;
  result.error = std::string(kNotImpl);
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::GetEntity(DatabaseID, uint16_t, std::function<void(GetResult)> callback) {
  GetResult result;
  result.error = std::string(kNotImpl);
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::DelEntity(DatabaseID, uint16_t, std::function<void(DelResult)> callback) {
  DelResult result;
  result.error = std::string(kNotImpl);
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::LookupByName(uint16_t, const std::string&,
                                 std::function<void(LookupResult)> callback) {
  LookupResult result;
  result.error = std::string(kNotImpl);
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::CheckoutEntity(DatabaseID, uint16_t, const CheckoutInfo&,
                                   std::function<void(GetResult)> callback) {
  GetResult result;
  result.error = std::string(kNotImpl);
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::CheckoutEntityByName(uint16_t, const std::string&, const CheckoutInfo&,
                                         std::function<void(GetResult)> callback) {
  GetResult result;
  result.error = std::string(kNotImpl);
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::ClearCheckout(DatabaseID, uint16_t, std::function<void(bool)> callback) {
  FireOrDefer([cb = std::move(callback)]() mutable { cb(false); });
}

void MysqlDatabase::ClearCheckoutsForAddress(const Address&, std::function<void(int)> callback) {
  FireOrDefer([cb = std::move(callback)]() mutable { cb(0); });
}

void MysqlDatabase::GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) {
  FireOrDefer([cb = std::move(callback)]() mutable { cb({}); });
}

void MysqlDatabase::SetAutoLoad(DatabaseID, uint16_t, bool) {}

void MysqlDatabase::GetMaxDbidInRange(DatabaseID, DatabaseID,
                                      std::function<void(DbidRangeResult)> callback) {
  DbidRangeResult result;
  result.error = std::string(kNotImpl);
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) {
  FireOrDefer([cb = std::move(callback)]() mutable { cb(1); });
}

void MysqlDatabase::SaveEntityIdCounter(EntityID, std::function<void(bool success)> callback) {
  FireOrDefer([cb = std::move(callback)]() mutable { cb(false); });
}

}  // namespace atlas
