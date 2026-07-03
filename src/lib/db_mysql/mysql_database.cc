#include "db_mysql/mysql_database.h"

#include <mysql.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <utility>

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

// ── lifecycle ───────────────────────────────────────────────────────────────

auto MysqlDatabase::Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void> {
  entity_defs_ = &entity_defs;
  config_ = config;

  conn_ = OpenConnection();
  if (conn_ == nullptr) {
    return Error{ErrorCode::kIoError,
                 std::format("MysqlDatabase: cannot connect to {}:{}/{}", config.mysql_host,
                             config.mysql_port, config.mysql_database)};
  }

  auto schema = EnsureSchema(conn_);
  if (!schema) {
    mysql_close(conn_);
    conn_ = nullptr;
    return schema.Error();
  }

  const int n = std::max(1, config.mysql_pool_size);
  for (int i = 0; i < n; ++i) {
    workers_.push_back(std::make_unique<Worker>());
  }
  for (auto& w : workers_) {
    Worker* wp = w.get();
    wp->thread = std::thread([this, wp] { WorkerLoop(*wp); });
  }

  started_ = true;
  ATLAS_LOG_INFO("MysqlDatabase: connected to {}:{}/{} ({} workers)", config.mysql_host,
                 config.mysql_port, config.mysql_database, n);
  return {};
}

void MysqlDatabase::Shutdown() {
  for (auto& w : workers_) {
    {
      std::lock_guard<std::mutex> lock(w->mutex);
      w->stop = true;
    }
    w->cv.notify_all();
  }
  for (auto& w : workers_) {
    if (w->thread.joinable()) w->thread.join();
  }
  workers_.clear();

  if (conn_ != nullptr) {
    mysql_close(conn_);
    conn_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(results_mutex_);
    results_.clear();
  }
  started_ = false;
}

auto MysqlDatabase::OpenConnection() const -> st_mysql* {
  st_mysql* conn = mysql_init(nullptr);
  if (conn == nullptr) return nullptr;

  unsigned int connect_timeout = 10;
  mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout);
  mysql_options(conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  bool reconnect = true;  // let the connector recover an idle-dropped link
  mysql_options(conn, MYSQL_OPT_RECONNECT, &reconnect);

  if (mysql_real_connect(conn, config_.mysql_host.c_str(), config_.mysql_user.c_str(),
                         config_.mysql_password.c_str(), config_.mysql_database.c_str(),
                         config_.mysql_port, nullptr, 0) == nullptr) {
    ATLAS_LOG_ERROR("MysqlDatabase: connect failed: {}", mysql_error(conn));
    mysql_close(conn);
    return nullptr;
  }
  return conn;
}

auto MysqlDatabase::EnsureSchema(st_mysql* conn) -> Result<void> {
  // Indexes live inside CREATE TABLE so the whole schema is idempotent through
  // IF NOT EXISTS; MySQL has no CREATE INDEX IF NOT EXISTS. Columns are binary
  // so identifier comparison stays byte-exact, matching the SQLite backend.
  auto entities = ExecSql(conn,
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

  auto meta = ExecSql(conn,
                      "CREATE TABLE IF NOT EXISTS meta ("
                      "`key` VARBINARY(64) NOT NULL,"
                      "`value` VARBINARY(255) NOT NULL,"
                      "PRIMARY KEY (`key`)"
                      ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!meta) return meta.Error();

  auto version =
      ExecSql(conn, std::format("INSERT INTO meta(`key`,`value`) VALUES('schema_version','{}') "
                                "ON DUPLICATE KEY UPDATE `value`=VALUES(`value`)",
                                kMysqlSchemaVersion));
  if (!version) return version.Error();

  auto counter = ExecSql(conn,
                         "CREATE TABLE IF NOT EXISTS atlas_entity_id_counter ("
                         "id INT NOT NULL,"
                         "next_id BIGINT UNSIGNED NOT NULL DEFAULT 1,"
                         "PRIMARY KEY (id),"
                         "CHECK (id = 1)"
                         ") ENGINE=InnoDB DEFAULT CHARSET=binary");
  if (!counter) return counter.Error();

  auto seed = ExecSql(conn, "INSERT IGNORE INTO atlas_entity_id_counter(id, next_id) VALUES(1, 1)");
  if (!seed) return seed.Error();

  return {};
}

// ── worker pool ─────────────────────────────────────────────────────────────

void MysqlDatabase::WorkerLoop(Worker& worker) {
  st_mysql* conn = OpenConnection();
  int backoff_ms = 100;
  while (conn == nullptr) {
    {
      std::unique_lock<std::mutex> lock(worker.mutex);
      if (worker.stop) return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    backoff_ms = std::min(backoff_ms * 2, 5000);
    conn = OpenConnection();
  }

  for (;;) {
    std::function<void(st_mysql*)> job;
    {
      std::unique_lock<std::mutex> lock(worker.mutex);
      worker.cv.wait(lock, [&] { return worker.stop || !worker.jobs.empty(); });
      if (worker.stop) break;
      job = std::move(worker.jobs.front());
      worker.jobs.pop_front();
    }
    job(conn);  // MYSQL_OPT_RECONNECT recovers a dropped link on the next query
  }

  mysql_close(conn);
}

void MysqlDatabase::EnqueueForEntity(uint16_t type_id, uint64_t key,
                                     std::function<void(st_mysql*)> job) {
  if (workers_.empty()) {
    job(conn_);  // no pool (pre-Startup / post-Shutdown fallback)
    return;
  }
  const std::size_t idx =
      static_cast<std::size_t>(key * 1099511628211ULL + type_id) % workers_.size();
  Worker& w = *workers_[idx];
  {
    std::lock_guard<std::mutex> lock(w.mutex);
    w.jobs.push_back(std::move(job));
  }
  w.cv.notify_one();
}

void MysqlDatabase::PostResult(std::function<void()> result) {
  std::lock_guard<std::mutex> lock(results_mutex_);
  results_.push_back(std::move(result));
}

void MysqlDatabase::ProcessResults() {
  for (int fired = 0; fired < kMaxCallbacksPerTick; ++fired) {
    std::function<void()> result;
    {
      std::lock_guard<std::mutex> lock(results_mutex_);
      if (results_.empty()) return;
      result = std::move(results_.front());
      results_.pop_front();
    }
    result();
  }
}

// ── SQL helpers (operate on a caller-supplied connection) ───────────────────

auto MysqlDatabase::ExecSql(st_mysql* conn, std::string_view sql) -> Result<void> {
  if (conn == nullptr) {
    return Error{ErrorCode::kIoError, "MysqlDatabase: not connected"};
  }
  if (mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return MysqlErr(conn, "MysqlDatabase: query failed");
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res != nullptr) {
    mysql_free_result(res);
  }
  return {};
}

auto MysqlDatabase::QueryRow(st_mysql* conn, std::string_view sql) -> Result<EntityRow> {
  if (conn == nullptr) {
    return Error{ErrorCode::kIoError, "MysqlDatabase: not connected"};
  }
  if (mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    return MysqlErr(conn, "MysqlDatabase: select failed");
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res == nullptr) {
    if (mysql_errno(conn) != 0) return MysqlErr(conn, "MysqlDatabase: store_result failed");
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

auto MysqlDatabase::ReadRow(char** row, const unsigned long* len) -> EntityRow {
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

auto MysqlDatabase::FetchByDbid(st_mysql* conn, DatabaseID dbid, uint16_t type_id)
    -> Result<EntityRow> {
  return QueryRow(
      conn, std::format("SELECT dbid, type_id, `blob`, identifier, password_hash, auto_load, "
                        "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid "
                        "FROM entities WHERE dbid = {} AND type_id = {} LIMIT 1",
                        static_cast<uint64_t>(dbid), type_id));
}

auto MysqlDatabase::FetchByName(st_mysql* conn, uint16_t type_id, std::string_view identifier)
    -> Result<EntityRow> {
  return QueryRow(
      conn, std::format("SELECT dbid, type_id, `blob`, identifier, password_hash, auto_load, "
                        "checked_out, checkout_ip, checkout_port, checkout_app_id, checkout_eid "
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

auto MysqlDatabase::MysqlErr(st_mysql* conn, std::string_view prefix) -> Error {
  const char* msg = (conn != nullptr) ? mysql_error(conn) : "no connection";
  return Error{ErrorCode::kIoError, std::format("{}: {}", prefix, msg)};
}

auto MysqlDatabase::ConstraintErrorKind(st_mysql* conn) -> DatabaseErrorKind {
  if (conn == nullptr || mysql_errno(conn) != kErDupEntry) {
    return DatabaseErrorKind::kNone;
  }
  const std::string_view msg = mysql_error(conn);
  return msg.find("PRIMARY") != std::string_view::npos ? DatabaseErrorKind::kDuplicateDbid
                                                       : DatabaseErrorKind::kDuplicateIdentifier;
}

auto MysqlDatabase::BeginTxn(st_mysql* conn) -> Result<void> {
  return ExecSql(conn, "START TRANSACTION");
}

void MysqlDatabase::Commit(st_mysql* conn) {
  if (auto r = ExecSql(conn, "COMMIT"); !r) {
    ATLAS_LOG_ERROR("MysqlDatabase: COMMIT failed: {}", r.Error().Message());
  }
}

void MysqlDatabase::Rollback(st_mysql* conn) {
  (void)ExecSql(conn, "ROLLBACK");
}

// ── operation bodies ────────────────────────────────────────────────────────

auto MysqlDatabase::DoPut(st_mysql* conn, DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                          std::span<const std::byte> blob, const std::string& identifier,
                          bool write_password, const std::string& password_hash) -> PutResult {
  PutResult result;
  if (HasFlag(flags, WriteFlags::kDelete)) {
    auto del = DoDel(conn, dbid, type_id);
    result.success = del.success;
    result.error = std::move(del.error);
    return result;
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
    auto exec = ExecSql(conn, sql);
    if (!exec) {
      result.error_kind = ConstraintErrorKind(conn);
      result.error = std::string(exec.Error().Message());
      return result;
    }
    result.success = true;
    result.dbid = explicit_dbid ? dbid : static_cast<DatabaseID>(mysql_insert_id(conn));
    return result;
  }

  auto row_result = FetchByDbid(conn, dbid, type_id);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    return result;
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

  auto exec = ExecSql(
      conn,
      std::format(
          "UPDATE entities SET `blob` = {}, identifier = {}, {}auto_load = {}, checked_out = "
          "{}, checkout_ip = {}, checkout_port = {}, checkout_app_id = {}, checkout_eid = {}, "
          "updated_at_ms = {} WHERE dbid = {} AND type_id = {}",
          HexLiteral(blob), IdentifierLiteral(final_identifier), password_set,
          final_auto_load ? 1 : 0, final_checked_out ? 1 : 0,
          final_checked_out ? owner.base_addr.Ip() : 0u,
          final_checked_out ? owner.base_addr.Port() : uint16_t{0},
          final_checked_out ? owner.app_id : 0u, final_checked_out ? owner.entity_id : 0u, now,
          static_cast<uint64_t>(dbid), type_id));
  if (!exec) {
    result.error = std::string(exec.Error().Message());
    return result;
  }
  result.success = true;
  result.dbid = dbid;
  return result;
}

auto MysqlDatabase::DoGet(st_mysql* conn, DatabaseID dbid, uint16_t type_id) -> GetResult {
  GetResult result;
  auto row_result = FetchByDbid(conn, dbid, type_id);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    return result;
  }
  result.success = true;
  result.data = std::move(row.data);
  result.checked_out_by = row.checked_out_by;
  return result;
}

auto MysqlDatabase::DoDel(st_mysql* conn, DatabaseID dbid, uint16_t type_id) -> DelResult {
  DelResult result;
  auto exec = ExecSql(conn, std::format("DELETE FROM entities WHERE dbid = {} AND type_id = {}",
                                        static_cast<uint64_t>(dbid), type_id));
  if (!exec) {
    result.error = std::string(exec.Error().Message());
    return result;
  }
  if (mysql_affected_rows(conn) == 0) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    return result;
  }
  result.success = true;
  return result;
}

auto MysqlDatabase::DoLookup(st_mysql* conn, uint16_t type_id, const std::string& identifier)
    -> LookupResult {
  LookupResult result;
  auto row_result = FetchByName(conn, type_id, identifier);
  if (!row_result) {
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (row.found) {
    result.found = true;
    result.dbid = row.data.dbid;
    result.password_hash = std::move(row.password_hash);
  }
  return result;
}

auto MysqlDatabase::CheckoutTail(st_mysql* conn, DatabaseID dbid, uint16_t type_id, EntityRow row,
                                 const CheckoutInfo& new_owner, int64_t now_ms) -> GetResult {
  GetResult result;
  // Conditional on checked_out = 0; a concurrent checkout that already flipped
  // it leaves 0 rows changed, which we treat as a conflict and re-fetch.
  auto upd = ExecSql(
      conn,
      std::format("UPDATE entities SET checked_out = 1, checkout_ip = {}, checkout_port = {}, "
                  "checkout_app_id = {}, checkout_eid = {}, updated_at_ms = {} "
                  "WHERE dbid = {} AND type_id = {} AND checked_out = 0",
                  new_owner.base_addr.Ip(), new_owner.base_addr.Port(), new_owner.app_id,
                  new_owner.entity_id, now_ms, static_cast<uint64_t>(dbid), type_id));
  if (!upd) {
    Rollback(conn);
    result.error = std::string(upd.Error().Message());
    return result;
  }
  if (mysql_affected_rows(conn) != 1) {
    Rollback(conn);
    auto refreshed = FetchByDbid(conn, dbid, type_id);
    if (refreshed && refreshed->found && refreshed->checked_out_by.has_value()) {
      result.success = true;
      result.data = std::move(refreshed->data);
      result.checked_out_by = refreshed->checked_out_by;
    } else {
      result.error = "checkout conflict";
    }
    return result;
  }
  Commit(conn);
  result.success = true;
  result.data = std::move(row.data);
  return result;
}

auto MysqlDatabase::DoCheckout(st_mysql* conn, DatabaseID dbid, uint16_t type_id,
                               const CheckoutInfo& new_owner) -> GetResult {
  GetResult result;
  const auto now = UnixTimeMs();
  if (auto begin = BeginTxn(conn); !begin) {
    result.error = std::string(begin.Error().Message());
    return result;
  }
  auto row_result = FetchByDbid(conn, dbid, type_id);
  if (!row_result) {
    Rollback(conn);
    result.error = std::string(row_result.Error().Message());
    return result;
  }
  auto row = std::move(*row_result);
  if (!row.found) {
    Rollback(conn);
    result.error = std::format("checkout: entity ({},{}) not found", type_id, dbid);
    return result;
  }
  if (row.checked_out_by.has_value()) {
    Rollback(conn);
    result.success = true;
    result.data = std::move(row.data);
    result.checked_out_by = row.checked_out_by;
    return result;
  }
  return CheckoutTail(conn, dbid, type_id, std::move(row), new_owner, now);
}

auto MysqlDatabase::DoCheckoutByName(st_mysql* conn, uint16_t type_id,
                                     const std::string& identifier, const CheckoutInfo& new_owner)
    -> GetResult {
  GetResult result;
  const auto now = UnixTimeMs();
  if (auto begin = BeginTxn(conn); !begin) {
    result.error = std::string(begin.Error().Message());
    return result;
  }
  auto lookup = FetchByName(conn, type_id, identifier);
  if (!lookup) {
    Rollback(conn);
    result.error = std::string(lookup.Error().Message());
    return result;
  }
  auto row = std::move(*lookup);
  if (!row.found) {
    Rollback(conn);
    result.error = std::format("checkout_by_name: '{}' not found", identifier);
    return result;
  }
  if (row.checked_out_by.has_value()) {
    Rollback(conn);
    result.success = true;
    result.data = std::move(row.data);
    result.checked_out_by = row.checked_out_by;
    return result;
  }
  const auto resolved_dbid = row.data.dbid;
  return CheckoutTail(conn, resolved_dbid, type_id, std::move(row), new_owner, now);
}

auto MysqlDatabase::DoClearCheckout(st_mysql* conn, DatabaseID dbid, uint16_t type_id) -> bool {
  auto upd = ExecSql(
      conn, std::format("UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
                        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = {} "
                        "WHERE dbid = {} AND type_id = {} AND checked_out = 1",
                        UnixTimeMs(), static_cast<uint64_t>(dbid), type_id));
  return upd && mysql_affected_rows(conn) > 0;
}

auto MysqlDatabase::DoClearForAddress(st_mysql* conn, const Address& base_addr) -> int {
  auto upd = ExecSql(
      conn, std::format("UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
                        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = {} "
                        "WHERE checked_out = 1 AND checkout_ip = {} AND checkout_port = {}",
                        UnixTimeMs(), base_addr.Ip(), base_addr.Port()));
  return upd ? static_cast<int>(mysql_affected_rows(conn)) : 0;
}

auto MysqlDatabase::DoGetAutoLoad(st_mysql* conn) -> std::vector<EntityData> {
  std::vector<EntityData> result;
  static constexpr std::string_view kSql =
      "SELECT dbid, type_id, `blob`, identifier FROM entities WHERE auto_load = 1 "
      "ORDER BY type_id, dbid";
  if (conn == nullptr ||
      mysql_real_query(conn, kSql.data(), static_cast<unsigned long>(kSql.size())) != 0) {
    return result;
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res == nullptr) return result;
  while (MYSQL_ROW row = mysql_fetch_row(res)) {
    const unsigned long* len = mysql_fetch_lengths(res);
    EntityData data;
    data.dbid = static_cast<DatabaseID>(std::strtoull(row[0], nullptr, 10));
    data.type_id = static_cast<uint16_t>(std::strtoul(row[1], nullptr, 10));
    if (row[2] != nullptr && len[2] > 0) {
      const auto* bytes = reinterpret_cast<const std::byte*>(row[2]);
      data.blob.assign(bytes, bytes + len[2]);
    }
    if (row[3] != nullptr) data.identifier.assign(row[3], len[3]);
    result.push_back(std::move(data));
  }
  mysql_free_result(res);
  return result;
}

void MysqlDatabase::DoSetAutoLoad(st_mysql* conn, DatabaseID dbid, uint16_t type_id,
                                  bool auto_load) {
  (void)ExecSql(conn,
                std::format("UPDATE entities SET auto_load = {}, updated_at_ms = {} "
                            "WHERE dbid = {} AND type_id = {}",
                            auto_load ? 1 : 0, UnixTimeMs(), static_cast<uint64_t>(dbid), type_id));
}

auto MysqlDatabase::DoGetMaxDbid(st_mysql* conn, DatabaseID low, DatabaseID high)
    -> DbidRangeResult {
  DbidRangeResult result;
  if (high <= low) {
    result.success = true;
    return result;
  }
  auto sql = std::format("SELECT MAX(dbid) FROM entities WHERE dbid >= {} AND dbid < {}",
                         static_cast<uint64_t>(low), static_cast<uint64_t>(high));
  if (conn == nullptr ||
      mysql_real_query(conn, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
    result.error = std::string(MysqlErr(conn, "MysqlDatabase: max dbid query failed").Message());
    return result;
  }
  MYSQL_RES* res = mysql_store_result(conn);
  if (res == nullptr) {
    result.error =
        std::string(MysqlErr(conn, "MysqlDatabase: max dbid store_result failed").Message());
    return result;
  }
  result.success = true;
  if (MYSQL_ROW row = mysql_fetch_row(res); row != nullptr && row[0] != nullptr) {
    result.max_dbid = static_cast<DatabaseID>(std::strtoull(row[0], nullptr, 10));
  }
  mysql_free_result(res);
  return result;
}

auto MysqlDatabase::DoLoadCounter(st_mysql* conn) -> EntityID {
  EntityID next_id = 1;
  static constexpr std::string_view kSql =
      "SELECT next_id FROM atlas_entity_id_counter WHERE id = 1";
  if (conn != nullptr &&
      mysql_real_query(conn, kSql.data(), static_cast<unsigned long>(kSql.size())) == 0) {
    if (MYSQL_RES* res = mysql_store_result(conn); res != nullptr) {
      if (MYSQL_ROW row = mysql_fetch_row(res); row != nullptr && row[0] != nullptr) {
        next_id = static_cast<EntityID>(std::strtoull(row[0], nullptr, 10));
      }
      mysql_free_result(res);
    }
  }
  return next_id;
}

auto MysqlDatabase::DoSaveCounter(st_mysql* conn, EntityID next_id) -> bool {
  // The counter row is seeded in EnsureSchema, so a successful UPDATE means it
  // was applied; MySQL reports 0 changed rows when the value is unchanged, so
  // affected_rows can't be the success signal.
  return ExecSql(conn, std::format("UPDATE atlas_entity_id_counter SET next_id = {} WHERE id = 1",
                                   static_cast<uint64_t>(next_id)))
      .HasValue();
}

// ── public API: dispatch onto the pool (or run inline when synchronous) ─────

void MysqlDatabase::PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                              std::span<const std::byte> blob, const std::string& identifier,
                              std::function<void(PutResult)> callback) {
  std::vector<std::byte> owned(blob.begin(), blob.end());
  Dispatch<PutResult>(
      type_id, dbid,
      [dbid, type_id, flags, b = std::move(owned), id = identifier](st_mysql* conn) {
        return DoPut(conn, dbid, type_id, flags, b, id, false, {});
      },
      std::move(callback));
}

void MysqlDatabase::PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                          std::span<const std::byte> blob,
                                          const std::string& identifier,
                                          const std::string& password_hash,
                                          std::function<void(PutResult)> callback) {
  std::vector<std::byte> owned(blob.begin(), blob.end());
  Dispatch<PutResult>(
      type_id, dbid,
      [dbid, type_id, flags, b = std::move(owned), id = identifier, pw = password_hash](
          st_mysql* conn) { return DoPut(conn, dbid, type_id, flags, b, id, true, pw); },
      std::move(callback));
}

void MysqlDatabase::GetEntity(DatabaseID dbid, uint16_t type_id,
                              std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      type_id, dbid, [dbid, type_id](st_mysql* conn) { return DoGet(conn, dbid, type_id); },
      std::move(callback));
}

void MysqlDatabase::DelEntity(DatabaseID dbid, uint16_t type_id,
                              std::function<void(DelResult)> callback) {
  Dispatch<DelResult>(
      type_id, dbid, [dbid, type_id](st_mysql* conn) { return DoDel(conn, dbid, type_id); },
      std::move(callback));
}

void MysqlDatabase::LookupByName(uint16_t type_id, const std::string& identifier,
                                 std::function<void(LookupResult)> callback) {
  Dispatch<LookupResult>(
      type_id, std::hash<std::string>{}(identifier),
      [type_id, id = identifier](st_mysql* conn) { return DoLookup(conn, type_id, id); },
      std::move(callback));
}

void MysqlDatabase::CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                                   std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      type_id, dbid,
      [dbid, type_id, new_owner](st_mysql* conn) {
        return DoCheckout(conn, dbid, type_id, new_owner);
      },
      std::move(callback));
}

void MysqlDatabase::CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                         const CheckoutInfo& new_owner,
                                         std::function<void(GetResult)> callback) {
  Dispatch<GetResult>(
      type_id, std::hash<std::string>{}(identifier),
      [type_id, id = identifier, new_owner](st_mysql* conn) {
        return DoCheckoutByName(conn, type_id, id, new_owner);
      },
      std::move(callback));
}

void MysqlDatabase::ClearCheckout(DatabaseID dbid, uint16_t type_id,
                                  std::function<void(bool)> callback) {
  Dispatch<bool>(
      type_id, dbid,
      [dbid, type_id](st_mysql* conn) { return DoClearCheckout(conn, dbid, type_id); },
      std::move(callback));
}

void MysqlDatabase::ClearCheckoutsForAddress(const Address& base_addr,
                                             std::function<void(int)> callback) {
  Dispatch<int>(
      0, 0, [base_addr](st_mysql* conn) { return DoClearForAddress(conn, base_addr); },
      std::move(callback));
}

void MysqlDatabase::GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) {
  Dispatch<std::vector<EntityData>>(
      0, 0, [](st_mysql* conn) { return DoGetAutoLoad(conn); }, std::move(callback));
}

void MysqlDatabase::SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) {
  DispatchVoid(type_id, dbid, [dbid, type_id, auto_load](st_mysql* conn) {
    DoSetAutoLoad(conn, dbid, type_id, auto_load);
  });
}

void MysqlDatabase::GetMaxDbidInRange(DatabaseID low, DatabaseID high,
                                      std::function<void(DbidRangeResult)> callback) {
  Dispatch<DbidRangeResult>(
      0, 0, [low, high](st_mysql* conn) { return DoGetMaxDbid(conn, low, high); },
      std::move(callback));
}

void MysqlDatabase::LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) {
  Dispatch<EntityID>(0, 0, [](st_mysql* conn) { return DoLoadCounter(conn); }, std::move(callback));
}

void MysqlDatabase::SaveEntityIdCounter(EntityID next_id,
                                        std::function<void(bool success)> callback) {
  Dispatch<bool>(
      0, 0, [next_id](st_mysql* conn) { return DoSaveCounter(conn, next_id); },
      std::move(callback));
}

}  // namespace atlas
