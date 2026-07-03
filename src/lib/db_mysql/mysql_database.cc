#include "db_mysql/mysql_database.h"

#include <mysql.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>

#include "foundation/log.h"

namespace atlas {

namespace {
constexpr int kMysqlSchemaVersion = 1;
constexpr unsigned int kErDupEntry = 1062;  // MySQL ER_DUP_ENTRY

auto UnixTimeMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
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
  if (conn_ == nullptr) {
    return Error{ErrorCode::kIoError, "MysqlDatabase: not connected"};
  }
  if (mysql_real_query(conn_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return MysqlError("MysqlDatabase: query failed");
  }
  // DDL/DML return no rows; free any result to keep the connection clean.
  MYSQL_RES* res = mysql_store_result(conn_);
  if (res != nullptr) {
    mysql_free_result(res);
  }
  return {};
}

auto MysqlDatabase::QueryRow(std::string_view sql) -> Result<EntityRow> {
  if (conn_ == nullptr) {
    return Error{ErrorCode::kIoError, "MysqlDatabase: not connected"};
  }
  if (mysql_real_query(conn_, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return MysqlError("MysqlDatabase: select failed");
  }
  MYSQL_RES* res = mysql_store_result(conn_);
  if (res == nullptr) {
    if (mysql_errno(conn_) != 0) return MysqlError("MysqlDatabase: store_result failed");
    return EntityRow{};
  }
  MYSQL_ROW row = mysql_fetch_row(res);
  if (row == nullptr) {
    mysql_free_result(res);
    return EntityRow{};
  }
  auto out = ReadRow(row, mysql_fetch_lengths(res));
  mysql_free_result(res);
  return out;
}

auto MysqlDatabase::ReadRow(char** row, const unsigned long* len) const -> EntityRow {
  EntityRow r;
  r.found = true;
  r.data.dbid = static_cast<DatabaseID>(std::strtoull(row[0], nullptr, 10));
  r.data.type_id = static_cast<uint16_t>(std::strtoul(row[1], nullptr, 10));
  if (row[2] != nullptr && len[2] > 0) {
    const auto* bytes = reinterpret_cast<const std::byte*>(row[2]);
    r.data.blob.assign(bytes, bytes + len[2]);
  }
  if (row[3] != nullptr) r.data.identifier.assign(row[3], len[3]);
  if (row[4] != nullptr) r.password_hash.assign(row[4], len[4]);
  r.auto_load = row[5] != nullptr && std::strtol(row[5], nullptr, 10) != 0;
  const bool checked_out = row[6] != nullptr && std::strtol(row[6], nullptr, 10) != 0;
  if (checked_out) {
    CheckoutInfo info;
    info.base_addr = Address(static_cast<uint32_t>(std::strtoul(row[7], nullptr, 10)),
                             static_cast<uint16_t>(std::strtoul(row[8], nullptr, 10)));
    info.app_id = static_cast<uint32_t>(std::strtoul(row[9], nullptr, 10));
    info.entity_id = static_cast<uint32_t>(std::strtoul(row[10], nullptr, 10));
    r.checked_out_by = info;
  }
  return r;
}

auto MysqlDatabase::FetchByDbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow> {
  return QueryRow(std::format(
      "SELECT dbid, type_id, `blob`, identifier, password_hash, auto_load, checked_out, "
      "checkout_ip, checkout_port, checkout_app_id, checkout_eid "
      "FROM entities WHERE dbid = {} AND type_id = {} LIMIT 1",
      static_cast<uint64_t>(dbid), type_id));
}

auto MysqlDatabase::FetchByName(uint16_t type_id, std::string_view identifier)
    -> Result<EntityRow> {
  return QueryRow(std::format(
      "SELECT dbid, type_id, `blob`, identifier, password_hash, auto_load, checked_out, "
      "checkout_ip, checkout_port, checkout_app_id, checkout_eid "
      "FROM entities WHERE type_id = {} AND identifier = {} LIMIT 1",
      type_id, IdentifierLiteral(identifier)));
}

auto MysqlDatabase::HexLiteral(std::span<const std::byte> data) -> std::string {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(data.size() * 2 + 3);
  out += "X'";
  for (auto b : data) {
    const auto v = static_cast<unsigned char>(b);
    out += kHex[v >> 4];
    out += kHex[v & 0x0F];
  }
  out += '\'';
  return out;
}

auto MysqlDatabase::IdentifierLiteral(std::string_view identifier) -> std::string {
  if (identifier.empty()) return "NULL";
  return HexLiteral(std::as_bytes(std::span<const char>(identifier.data(), identifier.size())));
}

auto MysqlDatabase::MysqlError(std::string_view prefix) const -> Error {
  const char* msg = (conn_ != nullptr) ? mysql_error(conn_) : "no connection";
  return Error{ErrorCode::kIoError, std::format("{}: {}", prefix, msg)};
}

auto MysqlDatabase::ConstraintErrorKind() const -> DatabaseErrorKind {
  if (conn_ == nullptr || mysql_errno(conn_) != kErDupEntry) {
    return DatabaseErrorKind::kNone;
  }
  const std::string_view msg = mysql_error(conn_);
  return msg.find("PRIMARY") != std::string_view::npos ? DatabaseErrorKind::kDuplicateDbid
                                                       : DatabaseErrorKind::kDuplicateIdentifier;
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

void MysqlDatabase::PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                              std::span<const std::byte> blob, const std::string& identifier,
                              std::function<void(PutResult)> callback) {
  PutInternal(dbid, type_id, flags, blob, identifier, false, {}, std::move(callback));
}

void MysqlDatabase::PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                          std::span<const std::byte> blob,
                                          const std::string& identifier,
                                          const std::string& password_hash,
                                          std::function<void(PutResult)> callback) {
  PutInternal(dbid, type_id, flags, blob, identifier, true, password_hash, std::move(callback));
}

void MysqlDatabase::PutInternal(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                std::span<const std::byte> blob, const std::string& identifier,
                                bool write_password, const std::string& password_hash,
                                std::function<void(PutResult)> callback) {
  PutResult result;
  if (!started_ || conn_ == nullptr) {
    result.error = "mysql backend not started";
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (HasFlag(flags, WriteFlags::kDelete)) {
    DelEntity(dbid, type_id, [cb = std::move(callback)](DelResult del) mutable {
      PutResult put;
      put.success = del.success;
      put.error = std::move(del.error);
      cb(std::move(put));
    });
    return;
  }

  const std::string password_literal = write_password ? IdentifierLiteral(password_hash) : "NULL";
  const auto now = UnixTimeMs();

  if (HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID) {
    const bool explicit_dbid = HasFlag(flags, WriteFlags::kExplicitDbid) && dbid != kInvalidDBID;
    const int auto_load = HasFlag(flags, WriteFlags::kAutoLoadOn) ? 1 : 0;
    std::string sql;
    if (explicit_dbid) {
      sql = std::format(
          "INSERT INTO entities (dbid, type_id, `blob`, identifier, password_hash, auto_load, "
          "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid, created_at_ms, "
          "updated_at_ms) VALUES ({}, {}, {}, {}, {}, {}, 0, 0, 0, 0, 0, {}, {})",
          static_cast<uint64_t>(dbid), type_id, HexLiteral(blob), IdentifierLiteral(identifier),
          password_literal, auto_load, now, now);
    } else {
      sql = std::format(
          "INSERT INTO entities (type_id, `blob`, identifier, password_hash, auto_load, "
          "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid, created_at_ms, "
          "updated_at_ms) VALUES ({}, {}, {}, {}, {}, 0, 0, 0, 0, 0, {}, {})",
          type_id, HexLiteral(blob), IdentifierLiteral(identifier), password_literal, auto_load,
          now, now);
    }
    auto exec = ExecSql(sql);
    if (!exec) {
      result.error_kind = ConstraintErrorKind();
      result.error = std::string(exec.Error().Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }
    result.success = true;
    result.dbid = explicit_dbid ? dbid : static_cast<DatabaseID>(mysql_insert_id(conn_));
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  // Update existing: fetch to carry forward checkout/auto_load state. Read-then-
  // write atomicity (transaction / SELECT FOR UPDATE) arrives with checkout in
  // P7.4.3; the single connection here has no concurrent writer yet.
  auto row_result = FetchByDbid(dbid, type_id);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  const std::string final_identifier = identifier.empty() ? row.data.identifier : identifier;
  const bool final_auto_load =
      HasFlag(flags, WriteFlags::kAutoLoadOn)
          ? true
          : (HasFlag(flags, WriteFlags::kAutoLoadOff) ? false : row.auto_load);
  const bool final_checked_out =
      HasFlag(flags, WriteFlags::kLogOff) ? false : row.checked_out_by.has_value();
  const CheckoutInfo owner = row.checked_out_by.value_or(CheckoutInfo{});
  const std::string password_set =
      write_password ? std::format("password_hash = {}, ", password_literal) : "";

  auto exec = ExecSql(std::format(
      "UPDATE entities SET `blob` = {}, identifier = {}, {}auto_load = {}, checked_out = {}, "
      "checkout_ip = {}, checkout_port = {}, checkout_app_id = {}, checkout_eid = {}, "
      "updated_at_ms = {} WHERE dbid = {} AND type_id = {}",
      HexLiteral(blob), IdentifierLiteral(final_identifier), password_set, final_auto_load ? 1 : 0,
      final_checked_out ? 1 : 0, final_checked_out ? owner.base_addr.Ip() : 0u,
      final_checked_out ? owner.base_addr.Port() : uint16_t{0},
      final_checked_out ? owner.app_id : 0u, final_checked_out ? owner.entity_id : 0u, now,
      static_cast<uint64_t>(dbid), type_id));
  if (!exec) {
    result.error = std::string(exec.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  result.success = true;
  result.dbid = dbid;
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::GetEntity(DatabaseID dbid, uint16_t type_id,
                              std::function<void(GetResult)> callback) {
  GetResult result;
  if (!started_ || conn_ == nullptr) {
    result.error = "mysql backend not started";
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto row_result = FetchByDbid(dbid, type_id);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  result.success = true;
  result.data = std::move(row.data);
  result.checked_out_by = row.checked_out_by;
  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void MysqlDatabase::DelEntity(DatabaseID dbid, uint16_t type_id,
                              std::function<void(DelResult)> callback) {
  DelResult result;
  if (!started_ || conn_ == nullptr) {
    result.error = "mysql backend not started";
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto exec = ExecSql(std::format("DELETE FROM entities WHERE dbid = {} AND type_id = {}",
                                  static_cast<uint64_t>(dbid), type_id));
  if (!exec) {
    result.error = std::string(exec.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  if (mysql_affected_rows(conn_) == 0) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  result.success = true;
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void MysqlDatabase::LookupByName(uint16_t type_id, const std::string& identifier,
                                 std::function<void(LookupResult)> callback) {
  LookupResult result;
  if (!started_ || conn_ == nullptr) {
    result.error = "mysql backend not started";
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto row_result = FetchByName(type_id, identifier);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  auto row = std::move(*row_result);
  if (row.found) {
    result.found = true;
    result.dbid = row.data.dbid;
    result.password_hash = std::move(row.password_hash);
  }
  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

// ── P7.4.3 stubs: checkout / auto-load / dbid range / id counter ────────────
namespace {
constexpr std::string_view kNotImpl = "MysqlDatabase: not implemented (P7.4.3)";
}  // namespace

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
