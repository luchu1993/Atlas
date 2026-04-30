#include "db_sqlite/sqlite_database.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>

#include "foundation/log.h"

namespace {

auto UnixTimeMs() -> int64_t {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int kSqliteSchemaVersion = 1;

}  // namespace

namespace atlas {

SqliteDatabase::Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_) {
  other.stmt_ = nullptr;
}

auto SqliteDatabase::Statement::operator=(Statement&& other) noexcept -> Statement& {
  if (this == &other) {
    return *this;
  }
  Reset();
  stmt_ = other.stmt_;
  other.stmt_ = nullptr;
  return *this;
}

SqliteDatabase::Statement::~Statement() {
  Reset();
}

void SqliteDatabase::Statement::Reset() {
  if (stmt_ != nullptr) {
    sqlite3_finalize(stmt_);
  }
  stmt_ = nullptr;
}

SqliteDatabase::~SqliteDatabase() {
  if (started_) {
    Shutdown();
  }
}

auto SqliteDatabase::Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void> {
  entity_defs_ = &entity_defs;
  db_path_ = config.sqlite_path;

  auto open_result = OpenDatabase(config);
  if (!open_result) {
    return open_result.Error();
  }

  auto schema_result = EnsureSchema();
  if (!schema_result) {
    Shutdown();
    return schema_result.Error();
  }

  started_ = true;
  ATLAS_LOG_INFO("SqliteDatabase: started at '{}'", db_path_.string());
  return {};
}

void SqliteDatabase::Shutdown() {
  if (db_ != nullptr) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
  deferred_.clear();
  started_ = false;
}

void SqliteDatabase::PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                               std::span<const std::byte> blob, const std::string& identifier,
                               std::function<void(PutResult)> callback) {
  PutResult result;

  if (!started_ || db_ == nullptr) {
    result.error = "sqlite backend not started";
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

  if (HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID) {
    const auto kNowMs = UnixTimeMs();
    auto stmt_result = Prepare(
        "INSERT INTO entities "
        "(type_id, blob, identifier, password_hash, auto_load, checked_out, "
        "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
        "created_at_ms, updated_at_ms) "
        "VALUES (?, ?, ?, NULL, ?, 0, 0, 0, 0, 0, ?, ?)");
    if (!stmt_result) {
      result.error = std::string(stmt_result.Error().Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int(stmt.Get(), 1, static_cast<int>(type_id));
    if (rc == SQLITE_OK) {
      auto bind_result = BindBlob(stmt, 2, blob);
      rc = bind_result ? SQLITE_OK : 1;
      if (!bind_result) result.error = std::string(bind_result.Error().Message());
    }
    if (rc == SQLITE_OK) {
      auto bind_result = BindIdentifier(stmt, 3, identifier);
      rc = bind_result ? SQLITE_OK : 1;
      if (!bind_result) result.error = std::string(bind_result.Error().Message());
    }
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), 4, HasFlag(flags, WriteFlags::kAutoLoadOn) ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 5, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 6, kNowMs);

    if (rc != SQLITE_OK) {
      if (result.error.empty())
        result.error = std::string(SqliteError("SqliteDatabase: insert bind failed").Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }

    rc = sqlite3_step(stmt.Get());
    if (rc != SQLITE_DONE) {
      result.error = std::string(SqliteError("SqliteDatabase: insert failed", rc).Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }

    result.success = true;
    result.dbid = static_cast<DatabaseID>(sqlite3_last_insert_rowid(db_));
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  // Wrap read-then-update in a write scope so checkout state cannot be
  // modified between the fetch and the write.  In batch mode this uses a
  // SAVEPOINT inside the per-tick transaction; otherwise a standalone
  // BEGIN IMMEDIATE / COMMIT.
  auto scope_result = BeginWriteScope();
  if (!scope_result) {
    result.error = std::string(scope_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  auto scope = std::move(*scope_result);

  auto row_result = FetchByDbid(dbid, type_id);
  if (!row_result) {
    RollbackWriteScope(scope);
    result.error = std::string(row_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto row = std::move(*row_result);
  if (!row.found) {
    RollbackWriteScope(scope);
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  const std::string kFinalIdentifier = identifier.empty() ? row.data.identifier : identifier;
  const bool kFinalAutoLoad =
      HasFlag(flags, WriteFlags::kAutoLoadOn)
          ? true
          : (HasFlag(flags, WriteFlags::kAutoLoadOff) ? false : row.auto_load);
  const bool kFinalCheckedOut =
      HasFlag(flags, WriteFlags::kLogOff) ? false : row.checked_out_by.has_value();
  const CheckoutInfo kFinalOwner = row.checked_out_by.value_or(CheckoutInfo{});
  const auto kNowMs = UnixTimeMs();

  auto stmt_result = Prepare(
      "UPDATE entities SET blob = ?, identifier = ?, auto_load = ?, checked_out = ?, "
      "checkout_ip = ?, checkout_port = ?, checkout_app_id = ?, checkout_eid = ?, "
      "updated_at_ms = ? WHERE dbid = ? AND type_id = ?");
  if (!stmt_result) {
    RollbackWriteScope(scope);
    result.error = std::string(stmt_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = SQLITE_OK;
  auto bind_blob_result = BindBlob(stmt, 1, blob);
  rc = bind_blob_result ? SQLITE_OK : 1;
  if (!bind_blob_result) result.error = std::string(bind_blob_result.Error().Message());
  if (rc == SQLITE_OK) {
    auto bind_result = BindIdentifier(stmt, 2, kFinalIdentifier);
    rc = bind_result ? SQLITE_OK : 1;
    if (!bind_result) result.error = std::string(bind_result.Error().Message());
  }
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, kFinalAutoLoad ? 1 : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, kFinalCheckedOut ? 1 : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int64(stmt.Get(), 5, kFinalCheckedOut ? kFinalOwner.base_addr.Ip() : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int(stmt.Get(), 6,
                          kFinalCheckedOut ? static_cast<int>(kFinalOwner.base_addr.Port()) : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int(stmt.Get(), 7,
                          kFinalCheckedOut ? static_cast<int>(kFinalOwner.app_id) : 0);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int(stmt.Get(), 8,
                          kFinalCheckedOut ? static_cast<int>(kFinalOwner.entity_id) : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 9, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 10, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 11, static_cast<int>(type_id));

  if (rc != SQLITE_OK) {
    RollbackWriteScope(scope);
    if (result.error.empty())
      result.error = std::string(SqliteError("SqliteDatabase: update bind failed").Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    RollbackWriteScope(scope);
    result.error = std::string(SqliteError("SqliteDatabase: update failed", rc).Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  CommitWriteScope(scope);

  result.success = true;
  result.dbid = dbid;
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void SqliteDatabase::PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                           std::span<const std::byte> blob,
                                           const std::string& identifier,
                                           const std::string& password_hash,
                                           std::function<void(PutResult)> callback) {
  PutResult result;

  if (!started_ || db_ == nullptr) {
    result.error = "sqlite backend not started";
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (!(HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID)) {
    auto scope_result = BeginWriteScope();
    if (!scope_result) {
      result.error = std::string(scope_result.Error().Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }
    auto scope = std::move(*scope_result);

    auto row_result = FetchByDbid(dbid, type_id);
    if (!row_result) {
      RollbackWriteScope(scope);
      result.error = std::string(row_result.Error().Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }
    if (!row_result->found) {
      RollbackWriteScope(scope);
      result.error = std::format("entity ({},{}) not found", type_id, dbid);
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }

    const std::string kFinalIdentifier =
        identifier.empty() ? row_result->data.identifier : identifier;
    const bool kFinalAutoLoad =
        HasFlag(flags, WriteFlags::kAutoLoadOn)
            ? true
            : (HasFlag(flags, WriteFlags::kAutoLoadOff) ? false : row_result->auto_load);
    const bool kFinalCheckedOut =
        HasFlag(flags, WriteFlags::kLogOff) ? false : row_result->checked_out_by.has_value();
    const CheckoutInfo kFinalOwner = row_result->checked_out_by.value_or(CheckoutInfo{});
    const auto kNowMs = UnixTimeMs();

    auto stmt_result = Prepare(
        "UPDATE entities SET blob = ?, identifier = ?, password_hash = ?, "
        "auto_load = ?, checked_out = ?, checkout_ip = ?, checkout_port = ?, "
        "checkout_app_id = ?, checkout_eid = ?, updated_at_ms = ? "
        "WHERE dbid = ? AND type_id = ?");
    if (!stmt_result) {
      RollbackWriteScope(scope);
      result.error = std::string(stmt_result.Error().Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = SQLITE_OK;
    auto bind_blob_result = BindBlob(stmt, 1, blob);
    rc = bind_blob_result ? SQLITE_OK : 1;
    if (!bind_blob_result) result.error = std::string(bind_blob_result.Error().Message());
    if (rc == SQLITE_OK) {
      auto bind_identifier_result = BindIdentifier(stmt, 2, kFinalIdentifier);
      rc = bind_identifier_result ? SQLITE_OK : 1;
      if (!bind_identifier_result)
        result.error = std::string(bind_identifier_result.Error().Message());
    }
    if (rc == SQLITE_OK) {
      if (password_hash.empty())
        rc = sqlite3_bind_null(stmt.Get(), 3);
      else
        rc = sqlite3_bind_text(stmt.Get(), 3, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, kFinalAutoLoad ? 1 : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 5, kFinalCheckedOut ? 1 : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int64(stmt.Get(), 6, kFinalCheckedOut ? kFinalOwner.base_addr.Ip() : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), 7,
                            kFinalCheckedOut ? static_cast<int>(kFinalOwner.base_addr.Port()) : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), 8,
                            kFinalCheckedOut ? static_cast<int>(kFinalOwner.app_id) : 0);
    if (rc == SQLITE_OK)
      rc = sqlite3_bind_int(stmt.Get(), 9,
                            kFinalCheckedOut ? static_cast<int>(kFinalOwner.entity_id) : 0);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 10, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 11, dbid);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 12, static_cast<int>(type_id));

    if (rc != SQLITE_OK || sqlite3_step(stmt.Get()) != SQLITE_DONE) {
      RollbackWriteScope(scope);
      if (result.error.empty())
        result.error = std::string(SqliteError("SqliteDatabase: password update failed").Message());
      FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
      return;
    }

    CommitWriteScope(scope);

    result.success = true;
    result.dbid = dbid;
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "INSERT INTO entities "
      "(type_id, blob, identifier, password_hash, auto_load, checked_out, "
      "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
      "created_at_ms, updated_at_ms) "
      "VALUES (?, ?, ?, ?, ?, 0, 0, 0, 0, 0, ?, ?)");
  if (!stmt_result) {
    result.error = std::string(stmt_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int(stmt.Get(), 1, static_cast<int>(type_id));
  if (rc == SQLITE_OK) {
    auto bind_result = BindBlob(stmt, 2, blob);
    rc = bind_result ? SQLITE_OK : 1;
    if (!bind_result) result.error = std::string(bind_result.Error().Message());
  }
  if (rc == SQLITE_OK) {
    auto bind_result = BindIdentifier(stmt, 3, identifier);
    rc = bind_result ? SQLITE_OK : 1;
    if (!bind_result) result.error = std::string(bind_result.Error().Message());
  }
  if (rc == SQLITE_OK) {
    if (password_hash.empty())
      rc = sqlite3_bind_null(stmt.Get(), 4);
    else
      rc = sqlite3_bind_text(stmt.Get(), 4, password_hash.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_int(stmt.Get(), 5, HasFlag(flags, WriteFlags::kAutoLoadOn) ? 1 : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 6, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 7, kNowMs);

  if (rc != SQLITE_OK) {
    if (result.error.empty())
      result.error =
          std::string(SqliteError("SqliteDatabase: password insert bind failed").Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    result.error = std::string(SqliteError("SqliteDatabase: password insert failed", rc).Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  result.success = true;
  result.dbid = static_cast<DatabaseID>(sqlite3_last_insert_rowid(db_));
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void SqliteDatabase::GetEntity(DatabaseID dbid, uint16_t type_id,
                               std::function<void(GetResult)> callback) {
  GetResult result;
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

void SqliteDatabase::DelEntity(DatabaseID dbid, uint16_t type_id,
                               std::function<void(DelResult)> callback) {
  DelResult result;
  auto stmt_result = Prepare("DELETE FROM entities WHERE dbid = ? AND type_id = ?");
  if (!stmt_result) {
    result.error = std::string(stmt_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, static_cast<int>(type_id));
  if (rc != SQLITE_OK) {
    result.error = std::string(SqliteError("SqliteDatabase: delete bind failed").Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    result.error = std::string(SqliteError("SqliteDatabase: delete failed", rc).Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (sqlite3_changes(db_) <= 0) {
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  result.success = true;
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void SqliteDatabase::LookupByName(uint16_t type_id, const std::string& identifier,
                                  std::function<void(LookupResult)> callback) {
  LookupResult result;
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

void SqliteDatabase::CheckoutEntity(DatabaseID dbid, uint16_t type_id,
                                    const CheckoutInfo& new_owner,
                                    std::function<void(GetResult)> callback) {
  GetResult result;
  const auto kNowMs = UnixTimeMs();

  auto scope_result = BeginWriteScope();
  if (!scope_result) {
    result.error = std::string(scope_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  auto scope = std::move(*scope_result);

  auto row_result = FetchByDbid(dbid, type_id);
  if (!row_result) {
    RollbackWriteScope(scope);
    result.error = std::string(row_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto row = std::move(*row_result);
  if (!row.found) {
    RollbackWriteScope(scope);
    result.error = std::format("checkout: entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (row.checked_out_by.has_value()) {
    RollbackWriteScope(scope);
    result.success = true;
    result.data = std::move(row.data);
    result.checked_out_by = row.checked_out_by;
    FireOrDefer([cb = std::move(callback), result = std::move(result)]() mutable {
      cb(std::move(result));
    });
    return;
  }

  auto update_result = Prepare(
      "UPDATE entities SET checked_out = 1, checkout_ip = ?, checkout_port = ?, "
      "checkout_app_id = ?, checkout_eid = ?, updated_at_ms = ? "
      "WHERE dbid = ? AND type_id = ? AND checked_out = 0");
  if (!update_result) {
    RollbackWriteScope(scope);
    result.error = std::string(update_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto stmt = std::move(*update_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, new_owner.base_addr.Ip());
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, new_owner.base_addr.Port());
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, static_cast<int>(new_owner.app_id));
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, static_cast<int>(new_owner.entity_id));
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 5, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 6, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 7, static_cast<int>(type_id));

  if (rc != SQLITE_OK || sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    RollbackWriteScope(scope);
    result.error = std::string(SqliteError("SqliteDatabase: checkout update failed").Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (sqlite3_changes(db_) != 1) {
    RollbackWriteScope(scope);
    auto refreshed = FetchByDbid(dbid, type_id);
    if (refreshed && refreshed->found && refreshed->checked_out_by.has_value()) {
      result.success = true;
      result.data = std::move(refreshed->data);
      result.checked_out_by = refreshed->checked_out_by;
    } else {
      result.error = "checkout conflict";
    }
    FireOrDefer([cb = std::move(callback), result = std::move(result)]() mutable {
      cb(std::move(result));
    });
    return;
  }

  CommitWriteScope(scope);

  result.success = true;
  result.data = std::move(row.data);
  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void SqliteDatabase::CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                          const CheckoutInfo& new_owner,
                                          std::function<void(GetResult)> callback) {
  GetResult result;
  const auto kNowMs = UnixTimeMs();

  // Resolve name and checkout atomically within a single write scope so
  // a concurrent rename/delete cannot cause us to check out a stale row.
  auto scope_result = BeginWriteScope();
  if (!scope_result) {
    result.error = std::string(scope_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  auto scope = std::move(*scope_result);

  auto lookup = FetchByName(type_id, identifier);
  if (!lookup) {
    RollbackWriteScope(scope);
    result.error = std::string(lookup.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (!lookup->found) {
    RollbackWriteScope(scope);
    result.error = std::format("checkout_by_name: '{}' not found", identifier);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  const auto kDbid = lookup->data.dbid;

  if (lookup->checked_out_by.has_value()) {
    RollbackWriteScope(scope);
    result.success = true;
    result.data = std::move(lookup->data);
    result.checked_out_by = lookup->checked_out_by;
    FireOrDefer([cb = std::move(callback), result = std::move(result)]() mutable {
      cb(std::move(result));
    });
    return;
  }

  auto update_result = Prepare(
      "UPDATE entities SET checked_out = 1, checkout_ip = ?, checkout_port = ?, "
      "checkout_app_id = ?, checkout_eid = ?, updated_at_ms = ? "
      "WHERE dbid = ? AND type_id = ? AND checked_out = 0");
  if (!update_result) {
    RollbackWriteScope(scope);
    result.error = std::string(update_result.Error().Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto stmt = std::move(*update_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, new_owner.base_addr.Ip());
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, new_owner.base_addr.Port());
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, static_cast<int>(new_owner.app_id));
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, static_cast<int>(new_owner.entity_id));
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 5, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 6, kDbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 7, static_cast<int>(type_id));

  if (rc != SQLITE_OK || sqlite3_step(stmt.Get()) != SQLITE_DONE) {
    RollbackWriteScope(scope);
    result.error =
        std::string(SqliteError("SqliteDatabase: checkout_by_name update failed").Message());
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (sqlite3_changes(db_) != 1) {
    RollbackWriteScope(scope);
    auto refreshed = FetchByDbid(kDbid, type_id);
    if (refreshed && refreshed->found && refreshed->checked_out_by.has_value()) {
      result.success = true;
      result.data = std::move(refreshed->data);
      result.checked_out_by = refreshed->checked_out_by;
    } else {
      result.error = "checkout_by_name conflict";
    }
    FireOrDefer([cb = std::move(callback), result = std::move(result)]() mutable {
      cb(std::move(result));
    });
    return;
  }

  CommitWriteScope(scope);

  result.success = true;
  result.data = std::move(lookup->data);
  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void SqliteDatabase::ClearCheckout(DatabaseID dbid, uint16_t type_id,
                                   std::function<void(bool)> callback) {
  bool cleared = false;
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
      "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
      "WHERE dbid = ? AND type_id = ? AND checked_out = 1");
  if (stmt_result) {
    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.Get(), 1, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, dbid);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, static_cast<int>(type_id));
    if (rc == SQLITE_OK && sqlite3_step(stmt.Get()) == SQLITE_DONE) {
      cleared = sqlite3_changes(db_) > 0;
    }
  }

  FireOrDefer([cb = std::move(callback), cleared]() mutable { cb(cleared); });
}

void SqliteDatabase::ClearCheckoutsForAddress(const Address& base_addr,
                                              std::function<void(int cleared_count)> callback) {
  int cleared = 0;
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
      "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
      "WHERE checked_out = 1 AND checkout_ip = ? AND checkout_port = ?");
  if (stmt_result) {
    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.Get(), 1, kNowMs);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, base_addr.Ip());
    if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, base_addr.Port());
    if (rc == SQLITE_OK && sqlite3_step(stmt.Get()) == SQLITE_DONE) {
      cleared = sqlite3_changes(db_);
    }
  }

  FireOrDefer([cb = std::move(callback), cleared]() mutable { cb(cleared); });
}

void SqliteDatabase::MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) {
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
      "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
      "WHERE dbid = ? AND type_id = ? AND checked_out = 1");
  if (!stmt_result) {
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 3, static_cast<int>(type_id));
  if (rc == SQLITE_OK) {
    (void)sqlite3_step(stmt.Get());
  }
}

void SqliteDatabase::GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) {
  std::vector<EntityData> result;
  auto stmt_result = Prepare(
      "SELECT dbid, type_id, blob, identifier FROM entities WHERE auto_load = 1 "
      "ORDER BY type_id, dbid");
  if (!stmt_result) {
    FireOrDefer([cb = std::move(callback), result = std::move(result)]() mutable {
      cb(std::move(result));
    });
    return;
  }

  auto stmt = std::move(*stmt_result);
  for (;;) {
    auto rc = sqlite3_step(stmt.Get());
    if (rc == SQLITE_DONE) {
      break;
    }
    if (rc != SQLITE_ROW) {
      break;
    }

    EntityData data;
    data.dbid = static_cast<DatabaseID>(sqlite3_column_int64(stmt.Get(), 0));
    data.type_id = static_cast<uint16_t>(sqlite3_column_int(stmt.Get(), 1));
    auto blob_ptr = sqlite3_column_blob(stmt.Get(), 2);
    auto blob_size = sqlite3_column_bytes(stmt.Get(), 2);
    if (blob_ptr != nullptr && blob_size > 0) {
      auto* bytes = static_cast<const std::byte*>(blob_ptr);
      data.blob.assign(bytes, bytes + blob_size);
    }
    if (const auto* text = sqlite3_column_text(stmt.Get(), 3); text != nullptr) {
      data.identifier = reinterpret_cast<const char*>(text);
    }
    result.push_back(std::move(data));
  }

  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void SqliteDatabase::SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) {
  const auto kNowMs = UnixTimeMs();
  auto stmt_result = Prepare(
      "UPDATE entities SET auto_load = ?, updated_at_ms = ? WHERE dbid = ? AND type_id = ?");
  if (!stmt_result) {
    return;
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int(stmt.Get(), 1, auto_load ? 1 : 0);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 2, kNowMs);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int64(stmt.Get(), 3, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 4, static_cast<int>(type_id));
  if (rc == SQLITE_OK) {
    (void)sqlite3_step(stmt.Get());
  }
}

void SqliteDatabase::LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) {
  EntityID next_id = 1;

  auto stmt_result = Prepare("SELECT next_id FROM atlas_entity_id_counter WHERE id = 1");
  if (stmt_result) {
    auto stmt = std::move(*stmt_result);
    if (sqlite3_step(stmt.Get()) == SQLITE_ROW) {
      next_id = static_cast<EntityID>(sqlite3_column_int64(stmt.Get(), 0));
    }
  }

  FireOrDefer([cb = std::move(callback), next_id]() { cb(next_id); });
}

void SqliteDatabase::SaveEntityIdCounter(EntityID next_id,
                                         std::function<void(bool success)> callback) {
  bool ok = false;
  auto stmt_result = Prepare("UPDATE atlas_entity_id_counter SET next_id = ? WHERE id = 1");
  if (stmt_result) {
    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.Get(), 1, static_cast<int64_t>(next_id));
    if (rc == SQLITE_OK && sqlite3_step(stmt.Get()) == SQLITE_DONE) {
      ok = (sqlite3_changes(db_) > 0);
    }
  }

  FireOrDefer([cb = std::move(callback), ok]() { cb(ok); });
}

void SqliteDatabase::ProcessResults() {
  if (!deferred_mode_) {
    return;
  }

  int budget = kMaxCallbacksPerTick;
  while (!deferred_.empty() && budget-- > 0) {
    auto cb = std::move(deferred_.front());
    deferred_.pop_front();
    cb();
  }
}

void SqliteDatabase::BeginBatch() {
  batch_active_ = true;
  batch_savepoint_seq_ = 0;
}

void SqliteDatabase::EndBatch() {
  if (batch_active_ && batch_txn_open_) {
    auto r = ExecSql("COMMIT");
    if (!r) {
      ATLAS_LOG_ERROR("SqliteDatabase: batch COMMIT failed: {}", r.Error().Message());
    }
    batch_txn_open_ = false;
  }
  batch_active_ = false;
  batch_savepoint_seq_ = 0;
}

auto SqliteDatabase::BeginWriteScope() -> Result<std::string> {
  if (!batch_active_) {
    auto r = ExecSql("BEGIN IMMEDIATE");
    if (!r) return r.Error();
    return std::string{};
  }

  if (!batch_txn_open_) {
    auto r = ExecSql("BEGIN IMMEDIATE");
    if (!r) return r.Error();
    batch_txn_open_ = true;
  }

  auto name = std::format("sp_{}", ++batch_savepoint_seq_);
  auto r = ExecSql(std::format("SAVEPOINT {}", name));
  if (!r) return r.Error();
  return name;
}

void SqliteDatabase::CommitWriteScope(const std::string& scope) {
  if (scope.empty()) {
    auto r = ExecSql("COMMIT");
    if (!r) {
      ATLAS_LOG_ERROR("SqliteDatabase: COMMIT failed: {}", r.Error().Message());
    }
  } else {
    auto r = ExecSql(std::format("RELEASE {}", scope));
    if (!r) {
      ATLAS_LOG_ERROR("SqliteDatabase: RELEASE {} failed: {}", scope, r.Error().Message());
    }
  }
}

void SqliteDatabase::RollbackWriteScope(const std::string& scope) {
  if (scope.empty()) {
    (void)ExecSql("ROLLBACK");
  } else {
    (void)ExecSql(std::format("ROLLBACK TO {}", scope));
    (void)ExecSql(std::format("RELEASE {}", scope));
  }
}

auto SqliteDatabase::OpenDatabase(const DatabaseConfig& config) -> Result<void> {
  auto parent = db_path_.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return Error{ErrorCode::kIoError, std::format("SqliteDatabase: cannot create dir '{}': {}",
                                                    parent.string(), ec.message())};
    }
  }

  auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
  auto rc = sqlite3_open_v2(db_path_.string().c_str(), &db_, flags, nullptr);
  if (rc != SQLITE_OK || db_ == nullptr) {
    return SqliteError("SqliteDatabase: sqlite3_open_v2 failed", rc);
  }

  rc = sqlite3_busy_timeout(db_, config.sqlite_busy_timeout_ms);
  if (rc != SQLITE_OK) {
    auto err = SqliteError("SqliteDatabase: sqlite3_busy_timeout failed", rc);
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  auto journal_sql =
      std::string(config.sqlite_wal ? "PRAGMA journal_mode = WAL" : "PRAGMA journal_mode = DELETE");
  auto pragma_result = ExecSql(journal_sql);
  if (!pragma_result) {
    auto err = pragma_result.Error();
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  auto fk_result = ExecSql(std::string(config.sqlite_foreign_keys ? "PRAGMA foreign_keys = ON"
                                                                  : "PRAGMA foreign_keys = OFF"));
  if (!fk_result) {
    auto err = fk_result.Error();
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  auto sync_result = ExecSql("PRAGMA synchronous = NORMAL");
  if (!sync_result) {
    auto err = sync_result.Error();
    sqlite3_close(db_);
    db_ = nullptr;
    return err;
  }

  return {};
}

auto SqliteDatabase::EnsureSchema() -> Result<void> {
  auto schema_result = ExecSql(
      "CREATE TABLE IF NOT EXISTS entities ("
      "dbid INTEGER PRIMARY KEY AUTOINCREMENT,"
      "type_id INTEGER NOT NULL,"
      "blob BLOB NOT NULL,"
      "identifier TEXT,"
      "password_hash TEXT,"
      "auto_load INTEGER NOT NULL DEFAULT 0,"
      "checked_out INTEGER NOT NULL DEFAULT 0,"
      "checkout_ip INTEGER NOT NULL DEFAULT 0,"
      "checkout_port INTEGER NOT NULL DEFAULT 0,"
      "checkout_app_id INTEGER NOT NULL DEFAULT 0,"
      "checkout_eid INTEGER NOT NULL DEFAULT 0,"
      "created_at_ms INTEGER NOT NULL DEFAULT 0,"
      "updated_at_ms INTEGER NOT NULL DEFAULT 0"
      ")");
  if (!schema_result) {
    return schema_result.Error();
  }

  auto created_at_result = ExecSqlIgnoringDuplicateColumn(
      "ALTER TABLE entities ADD COLUMN created_at_ms INTEGER NOT NULL DEFAULT 0");
  if (!created_at_result) {
    return created_at_result.Error();
  }

  auto updated_at_result = ExecSqlIgnoringDuplicateColumn(
      "ALTER TABLE entities ADD COLUMN updated_at_ms INTEGER NOT NULL DEFAULT 0");
  if (!updated_at_result) {
    return updated_at_result.Error();
  }

  auto meta_table_result =
      ExecSql("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
  if (!meta_table_result) {
    return meta_table_result.Error();
  }

  auto schema_version_result = UpsertMeta("schema_version", std::to_string(kSqliteSchemaVersion));
  if (!schema_version_result) {
    return schema_version_result.Error();
  }

  auto index1 =
      ExecSql("CREATE INDEX IF NOT EXISTS idx_entities_type_dbid ON entities(type_id, dbid)");
  if (!index1) {
    return index1.Error();
  }

  auto index2 = ExecSql(
      "CREATE INDEX IF NOT EXISTS idx_entities_type_identifier "
      "ON entities(type_id, identifier)");
  if (!index2) {
    return index2.Error();
  }

  auto index3 = ExecSql(
      "CREATE UNIQUE INDEX IF NOT EXISTS idx_entities_type_identifier_unique "
      "ON entities(type_id, identifier) "
      "WHERE identifier IS NOT NULL AND identifier <> ''");
  if (!index3) {
    return index3.Error();
  }

  auto index4 = ExecSql(
      "CREATE INDEX IF NOT EXISTS idx_entities_auto_load ON entities(auto_load, type_id, dbid)");
  if (!index4) {
    return index4.Error();
  }

  auto index5 = ExecSql(
      "CREATE INDEX IF NOT EXISTS idx_entities_checkout_addr "
      "ON entities(checked_out, checkout_ip, checkout_port)");
  if (!index5) {
    return index5.Error();
  }

  auto eid_table_result = ExecSql(
      "CREATE TABLE IF NOT EXISTS atlas_entity_id_counter ("
      "id INTEGER PRIMARY KEY CHECK(id = 1),"
      "next_id INTEGER NOT NULL DEFAULT 1"
      ")");
  if (!eid_table_result) {
    return eid_table_result.Error();
  }

  auto eid_seed_result =
      ExecSql("INSERT OR IGNORE INTO atlas_entity_id_counter(id, next_id) VALUES(1, 1)");
  if (!eid_seed_result) {
    return eid_seed_result.Error();
  }

  return {};
}

auto SqliteDatabase::ExecSql(std::string_view sql) -> Result<void> {
  char* err = nullptr;
  auto rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err);
  if (rc == SQLITE_OK) {
    return {};
  }

  std::string message = std::format("SqliteDatabase: exec failed '{}'", sql);
  if (err != nullptr) {
    message += ": ";
    message += err;
    sqlite3_free(err);
  } else if (db_ != nullptr) {
    message += ": ";
    message += sqlite3_errmsg(db_);
  }

  return Error{ErrorCode::kInternalError, std::move(message)};
}

auto SqliteDatabase::ExecSqlIgnoringDuplicateColumn(std::string_view sql) -> Result<void> {
  auto result = ExecSql(sql);
  if (result) {
    return result;
  }

  if (std::string_view(result.Error().Message()).find("duplicate column name") !=
      std::string_view::npos) {
    return {};
  }

  return result.Error();
}

auto SqliteDatabase::Prepare(std::string_view sql) -> Result<Statement> {
  sqlite3_stmt* stmt = nullptr;
  auto rc = sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &stmt, nullptr);
  if (rc != SQLITE_OK) {
    return SqliteError(std::format("SqliteDatabase: prepare failed '{}'", sql), rc);
  }
  return Statement{stmt};
}

auto SqliteDatabase::UpsertMeta(std::string_view key, std::string_view value) -> Result<void> {
  auto stmt_result = Prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
  if (!stmt_result) {
    return stmt_result.Error();
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_text(stmt.Get(), 1, std::string(key).c_str(), -1, SQLITE_TRANSIENT);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text(stmt.Get(), 2, std::string(value).c_str(), -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: meta bind failed", rc);
  }

  rc = sqlite3_step(stmt.Get());
  if (rc != SQLITE_DONE) {
    return SqliteError("SqliteDatabase: meta upsert failed", rc);
  }

  return {};
}

auto SqliteDatabase::FetchByDbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow> {
  auto stmt_result = Prepare(
      "SELECT dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
      "checkout_ip, checkout_port, checkout_app_id, checkout_eid "
      "FROM entities WHERE dbid = ? AND type_id = ? LIMIT 1");
  if (!stmt_result) {
    return stmt_result.Error();
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int64(stmt.Get(), 1, dbid);
  if (rc == SQLITE_OK) rc = sqlite3_bind_int(stmt.Get(), 2, static_cast<int>(type_id));
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: fetch_by_dbid bind failed", rc);
  }

  rc = sqlite3_step(stmt.Get());
  if (rc == SQLITE_DONE) {
    return EntityRow{};
  }
  if (rc != SQLITE_ROW) {
    return SqliteError("SqliteDatabase: fetch_by_dbid step failed", rc);
  }
  return ReadRow(stmt.Get());
}

auto SqliteDatabase::FetchByName(uint16_t type_id, std::string_view identifier)
    -> Result<EntityRow> {
  auto stmt_result = Prepare(
      "SELECT dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
      "checkout_ip, checkout_port, checkout_app_id, checkout_eid "
      "FROM entities WHERE type_id = ? AND identifier = ? LIMIT 1");
  if (!stmt_result) {
    return stmt_result.Error();
  }

  auto stmt = std::move(*stmt_result);
  auto rc = sqlite3_bind_int(stmt.Get(), 1, static_cast<int>(type_id));
  auto owned_identifier = std::string(identifier);
  if (rc == SQLITE_OK)
    rc = sqlite3_bind_text(stmt.Get(), 2, owned_identifier.c_str(), -1, SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: fetch_by_name bind failed", rc);
  }

  rc = sqlite3_step(stmt.Get());
  if (rc == SQLITE_DONE) {
    return EntityRow{};
  }
  if (rc != SQLITE_ROW) {
    return SqliteError("SqliteDatabase: fetch_by_name step failed", rc);
  }
  return ReadRow(stmt.Get());
}

auto SqliteDatabase::ReadRow(sqlite3_stmt* stmt) const -> EntityRow {
  EntityRow row;
  row.found = true;
  row.data.dbid = static_cast<DatabaseID>(sqlite3_column_int64(stmt, 0));
  row.data.type_id = static_cast<uint16_t>(sqlite3_column_int(stmt, 1));

  auto blob_ptr = sqlite3_column_blob(stmt, 2);
  auto blob_size = sqlite3_column_bytes(stmt, 2);
  if (blob_ptr != nullptr && blob_size > 0) {
    auto* bytes = static_cast<const std::byte*>(blob_ptr);
    row.data.blob.assign(bytes, bytes + blob_size);
  }

  if (const auto* text = sqlite3_column_text(stmt, 3); text != nullptr) {
    row.data.identifier = reinterpret_cast<const char*>(text);
  }
  if (const auto* text = sqlite3_column_text(stmt, 4); text != nullptr) {
    row.password_hash = reinterpret_cast<const char*>(text);
  }

  row.auto_load = sqlite3_column_int(stmt, 5) != 0;
  const bool kCheckedOut = sqlite3_column_int(stmt, 6) != 0;
  if (kCheckedOut) {
    CheckoutInfo info;
    info.base_addr = Address(static_cast<uint32_t>(sqlite3_column_int64(stmt, 7)),
                             static_cast<uint16_t>(sqlite3_column_int(stmt, 8)));
    info.app_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 9));
    info.entity_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
    row.checked_out_by = info;
  }

  return row;
}

auto SqliteDatabase::BindIdentifier(Statement& stmt, int index, const std::string& identifier) const
    -> Result<void> {
  int rc = SQLITE_OK;
  if (identifier.empty()) {
    rc = sqlite3_bind_null(stmt.Get(), index);
  } else {
    rc = sqlite3_bind_text(stmt.Get(), index, identifier.c_str(), -1, SQLITE_TRANSIENT);
  }
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: bind identifier failed", rc);
  }
  return {};
}

auto SqliteDatabase::BindBlob(Statement& stmt, int index, std::span<const std::byte> blob) const
    -> Result<void> {
  static const std::byte kEmptyBlob = std::byte{0};
  const void* data = blob.empty() ? &kEmptyBlob : blob.data();
  auto rc =
      sqlite3_bind_blob(stmt.Get(), index, data, static_cast<int>(blob.size()), SQLITE_TRANSIENT);
  if (rc != SQLITE_OK) {
    return SqliteError("SqliteDatabase: bind blob failed", rc);
  }
  return {};
}

auto SqliteDatabase::SqliteError(std::string_view prefix) const -> Error {
  std::string message(prefix);
  if (db_ != nullptr) {
    message += ": ";
    message += sqlite3_errmsg(db_);
  }
  return Error{ErrorCode::kInternalError, std::move(message)};
}

auto SqliteDatabase::SqliteError(std::string_view prefix, int code) const -> Error {
  return Error{ErrorCode::kInternalError,
               std::format("{} (sqlite rc={}){}", prefix, code,
                           db_ != nullptr ? std::format(": {}", sqlite3_errmsg(db_)) : "")};
}

void SqliteDatabase::FireOrDefer(std::function<void()> cb) {
  if (deferred_mode_) {
    deferred_.push_back(std::move(cb));
  } else {
    cb();
  }
}

}  // namespace atlas
