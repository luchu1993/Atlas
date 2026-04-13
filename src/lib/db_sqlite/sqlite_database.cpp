#include "db_sqlite/sqlite_database.hpp"

#include "foundation/log.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <string_view>

namespace
{

auto unix_time_ms() -> int64_t
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

constexpr int kSqliteSchemaVersion = 1;

}  // namespace

namespace atlas
{

SqliteDatabase::Statement::Statement(Statement&& other) noexcept : stmt_(other.stmt_)
{
    other.stmt_ = nullptr;
}

auto SqliteDatabase::Statement::operator=(Statement&& other) noexcept -> Statement&
{
    if (this == &other)
    {
        return *this;
    }
    reset();
    stmt_ = other.stmt_;
    other.stmt_ = nullptr;
    return *this;
}

SqliteDatabase::Statement::~Statement()
{
    reset();
}

void SqliteDatabase::Statement::reset()
{
    if (stmt_ != nullptr)
    {
        sqlite3_finalize(stmt_);
    }
    stmt_ = nullptr;
}

SqliteDatabase::~SqliteDatabase()
{
    if (started_)
    {
        shutdown();
    }
}

auto SqliteDatabase::startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void>
{
    entity_defs_ = &entity_defs;
    db_path_ = config.sqlite_path;

    auto open_result = open_database(config);
    if (!open_result)
    {
        return open_result.error();
    }

    auto schema_result = ensure_schema();
    if (!schema_result)
    {
        shutdown();
        return schema_result.error();
    }

    started_ = true;
    ATLAS_LOG_INFO("SqliteDatabase: started at '{}'", db_path_.string());
    return {};
}

void SqliteDatabase::shutdown()
{
    if (db_ != nullptr)
    {
        sqlite3_close(db_);
        db_ = nullptr;
    }
    deferred_.clear();
    started_ = false;
}

void SqliteDatabase::put_entity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                std::span<const std::byte> blob, const std::string& identifier,
                                std::function<void(PutResult)> callback)
{
    PutResult result;

    if (!started_ || db_ == nullptr)
    {
        result.error = "sqlite backend not started";
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (has_flag(flags, WriteFlags::Delete))
    {
        del_entity(dbid, type_id,
                   [cb = std::move(callback)](DelResult del) mutable
                   {
                       PutResult put;
                       put.success = del.success;
                       put.error = std::move(del.error);
                       cb(std::move(put));
                   });
        return;
    }

    if (has_flag(flags, WriteFlags::CreateNew) || dbid == kInvalidDBID)
    {
        const auto now_ms = unix_time_ms();
        auto stmt_result = prepare(
            "INSERT INTO entities "
            "(type_id, blob, identifier, password_hash, auto_load, checked_out, "
            "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
            "created_at_ms, updated_at_ms) "
            "VALUES (?, ?, ?, NULL, ?, 0, 0, 0, 0, 0, ?, ?)");
        if (!stmt_result)
        {
            result.error = std::string(stmt_result.error().message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        auto stmt = std::move(*stmt_result);
        auto rc = sqlite3_bind_int(stmt.get(), 1, static_cast<int>(type_id));
        if (rc == SQLITE_OK)
        {
            auto bind_result = bind_blob(stmt, 2, blob);
            rc = bind_result ? SQLITE_OK : 1;
            if (!bind_result)
                result.error = std::string(bind_result.error().message());
        }
        if (rc == SQLITE_OK)
        {
            auto bind_result = bind_identifier(stmt, 3, identifier);
            rc = bind_result ? SQLITE_OK : 1;
            if (!bind_result)
                result.error = std::string(bind_result.error().message());
        }
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 4, has_flag(flags, WriteFlags::AutoLoadOn) ? 1 : 0);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(stmt.get(), 5, now_ms);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(stmt.get(), 6, now_ms);

        if (rc != SQLITE_OK)
        {
            if (result.error.empty())
                result.error =
                    std::string(sqlite_error("SqliteDatabase: insert bind failed").message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        rc = sqlite3_step(stmt.get());
        if (rc != SQLITE_DONE)
        {
            result.error = std::string(sqlite_error("SqliteDatabase: insert failed", rc).message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        result.success = true;
        result.dbid = static_cast<DatabaseID>(sqlite3_last_insert_rowid(db_));
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto row_result = fetch_by_dbid(dbid, type_id);
    if (!row_result)
    {
        result.error = std::string(row_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto row = std::move(*row_result);
    if (!row.found)
    {
        result.error = std::format("entity ({},{}) not found", type_id, dbid);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    const std::string final_identifier = identifier.empty() ? row.data.identifier : identifier;
    const bool final_auto_load =
        has_flag(flags, WriteFlags::AutoLoadOn)
            ? true
            : (has_flag(flags, WriteFlags::AutoLoadOff) ? false : row.auto_load);
    const bool final_checked_out =
        has_flag(flags, WriteFlags::LogOff) ? false : row.checked_out_by.has_value();
    const CheckoutInfo final_owner = row.checked_out_by.value_or(CheckoutInfo{});
    const auto now_ms = unix_time_ms();

    auto stmt_result = prepare(
        "UPDATE entities SET blob = ?, identifier = ?, auto_load = ?, checked_out = ?, "
        "checkout_ip = ?, checkout_port = ?, checkout_app_id = ?, checkout_eid = ?, "
        "updated_at_ms = ? WHERE dbid = ? AND type_id = ?");
    if (!stmt_result)
    {
        result.error = std::string(stmt_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = SQLITE_OK;
    auto bind_blob_result = bind_blob(stmt, 1, blob);
    rc = bind_blob_result ? SQLITE_OK : 1;
    if (!bind_blob_result)
        result.error = std::string(bind_blob_result.error().message());
    if (rc == SQLITE_OK)
    {
        auto bind_result = bind_identifier(stmt, 2, final_identifier);
        rc = bind_result ? SQLITE_OK : 1;
        if (!bind_result)
            result.error = std::string(bind_result.error().message());
    }
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 3, final_auto_load ? 1 : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 4, final_checked_out ? 1 : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 5, final_checked_out ? final_owner.base_addr.ip() : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 6, final_checked_out ? final_owner.base_addr.port() : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 7, final_checked_out ? final_owner.app_id : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 8, final_checked_out ? final_owner.entity_id : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 9, now_ms);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 10, dbid);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 11, static_cast<int>(type_id));

    if (rc != SQLITE_OK)
    {
        if (result.error.empty())
            result.error =
                std::string(sqlite_error("SqliteDatabase: update bind failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE)
    {
        result.error = std::string(sqlite_error("SqliteDatabase: update failed", rc).message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    result.success = true;
    result.dbid = dbid;
    fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
}

void SqliteDatabase::put_entity_with_password(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                              std::span<const std::byte> blob,
                                              const std::string& identifier,
                                              const std::string& password_hash,
                                              std::function<void(PutResult)> callback)
{
    PutResult result;

    if (!started_ || db_ == nullptr)
    {
        result.error = "sqlite backend not started";
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (!(has_flag(flags, WriteFlags::CreateNew) || dbid == kInvalidDBID))
    {
        auto row_result = fetch_by_dbid(dbid, type_id);
        if (!row_result)
        {
            result.error = std::string(row_result.error().message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }
        if (!row_result->found)
        {
            result.error = std::format("entity ({},{}) not found", type_id, dbid);
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        const std::string final_identifier =
            identifier.empty() ? row_result->data.identifier : identifier;
        const bool final_auto_load =
            has_flag(flags, WriteFlags::AutoLoadOn)
                ? true
                : (has_flag(flags, WriteFlags::AutoLoadOff) ? false : row_result->auto_load);
        const bool final_checked_out =
            has_flag(flags, WriteFlags::LogOff) ? false : row_result->checked_out_by.has_value();
        const CheckoutInfo final_owner = row_result->checked_out_by.value_or(CheckoutInfo{});
        const auto now_ms = unix_time_ms();

        auto stmt_result = prepare(
            "UPDATE entities SET blob = ?, identifier = ?, password_hash = ?, "
            "auto_load = ?, checked_out = ?, checkout_ip = ?, checkout_port = ?, "
            "checkout_app_id = ?, checkout_eid = ?, updated_at_ms = ? "
            "WHERE dbid = ? AND type_id = ?");
        if (!stmt_result)
        {
            result.error = std::string(stmt_result.error().message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        auto stmt = std::move(*stmt_result);
        auto rc = SQLITE_OK;
        auto bind_blob_result = bind_blob(stmt, 1, blob);
        rc = bind_blob_result ? SQLITE_OK : 1;
        if (!bind_blob_result)
            result.error = std::string(bind_blob_result.error().message());
        if (rc == SQLITE_OK)
        {
            auto bind_identifier_result = bind_identifier(stmt, 2, final_identifier);
            rc = bind_identifier_result ? SQLITE_OK : 1;
            if (!bind_identifier_result)
                result.error = std::string(bind_identifier_result.error().message());
        }
        if (rc == SQLITE_OK)
        {
            if (password_hash.empty())
                rc = sqlite3_bind_null(stmt.get(), 3);
            else
                rc = sqlite3_bind_text(stmt.get(), 3, password_hash.c_str(), -1, SQLITE_TRANSIENT);
        }
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 4, final_auto_load ? 1 : 0);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 5, final_checked_out ? 1 : 0);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(stmt.get(), 6,
                                    final_checked_out ? final_owner.base_addr.ip() : 0);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 7,
                                  final_checked_out ? final_owner.base_addr.port() : 0);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 8, final_checked_out ? final_owner.app_id : 0);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 9, final_checked_out ? final_owner.entity_id : 0);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(stmt.get(), 10, now_ms);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(stmt.get(), 11, dbid);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 12, static_cast<int>(type_id));

        if (rc != SQLITE_OK || sqlite3_step(stmt.get()) != SQLITE_DONE)
        {
            if (result.error.empty())
                result.error =
                    std::string(sqlite_error("SqliteDatabase: password update failed").message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        result.success = true;
        result.dbid = dbid;
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    const auto now_ms = unix_time_ms();
    auto stmt_result = prepare(
        "INSERT INTO entities "
        "(type_id, blob, identifier, password_hash, auto_load, checked_out, "
        "checkout_ip, checkout_port, checkout_app_id, checkout_eid, "
        "created_at_ms, updated_at_ms) "
        "VALUES (?, ?, ?, ?, ?, 0, 0, 0, 0, 0, ?, ?)");
    if (!stmt_result)
    {
        result.error = std::string(stmt_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int(stmt.get(), 1, static_cast<int>(type_id));
    if (rc == SQLITE_OK)
    {
        auto bind_result = bind_blob(stmt, 2, blob);
        rc = bind_result ? SQLITE_OK : 1;
        if (!bind_result)
            result.error = std::string(bind_result.error().message());
    }
    if (rc == SQLITE_OK)
    {
        auto bind_result = bind_identifier(stmt, 3, identifier);
        rc = bind_result ? SQLITE_OK : 1;
        if (!bind_result)
            result.error = std::string(bind_result.error().message());
    }
    if (rc == SQLITE_OK)
    {
        if (password_hash.empty())
            rc = sqlite3_bind_null(stmt.get(), 4);
        else
            rc = sqlite3_bind_text(stmt.get(), 4, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 5, has_flag(flags, WriteFlags::AutoLoadOn) ? 1 : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 6, now_ms);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 7, now_ms);

    if (rc != SQLITE_OK)
    {
        if (result.error.empty())
            result.error =
                std::string(sqlite_error("SqliteDatabase: password insert bind failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE)
    {
        result.error =
            std::string(sqlite_error("SqliteDatabase: password insert failed", rc).message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    result.success = true;
    result.dbid = static_cast<DatabaseID>(sqlite3_last_insert_rowid(db_));
    fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
}

void SqliteDatabase::get_entity(DatabaseID dbid, uint16_t type_id,
                                std::function<void(GetResult)> callback)
{
    GetResult result;
    auto row_result = fetch_by_dbid(dbid, type_id);
    if (!row_result)
    {
        result.error = std::string(row_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto row = std::move(*row_result);
    if (!row.found)
    {
        result.error = std::format("entity ({},{}) not found", type_id, dbid);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    result.success = true;
    result.data = std::move(row.data);
    result.checked_out_by = row.checked_out_by;
    fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                  { cb(std::move(result)); });
}

void SqliteDatabase::del_entity(DatabaseID dbid, uint16_t type_id,
                                std::function<void(DelResult)> callback)
{
    DelResult result;
    auto stmt_result = prepare("DELETE FROM entities WHERE dbid = ? AND type_id = ?");
    if (!stmt_result)
    {
        result.error = std::string(stmt_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.get(), 1, dbid);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 2, static_cast<int>(type_id));
    if (rc != SQLITE_OK)
    {
        result.error = std::string(sqlite_error("SqliteDatabase: delete bind failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE)
    {
        result.error = std::string(sqlite_error("SqliteDatabase: delete failed", rc).message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (sqlite3_changes(db_) <= 0)
    {
        result.error = std::format("entity ({},{}) not found", type_id, dbid);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    result.success = true;
    fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
}

void SqliteDatabase::lookup_by_name(uint16_t type_id, const std::string& identifier,
                                    std::function<void(LookupResult)> callback)
{
    LookupResult result;
    auto row_result = fetch_by_name(type_id, identifier);
    if (!row_result)
    {
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto row = std::move(*row_result);
    if (row.found)
    {
        result.found = true;
        result.dbid = row.data.dbid;
        result.password_hash = std::move(row.password_hash);
    }
    fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                  { cb(std::move(result)); });
}

void SqliteDatabase::checkout_entity(DatabaseID dbid, uint16_t type_id,
                                     const CheckoutInfo& new_owner,
                                     std::function<void(GetResult)> callback)
{
    GetResult result;
    const auto now_ms = unix_time_ms();

    auto begin_result = exec_sql("BEGIN IMMEDIATE");
    if (!begin_result)
    {
        result.error = std::string(begin_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto row_result = fetch_by_dbid(dbid, type_id);
    if (!row_result)
    {
        (void)exec_sql("ROLLBACK");
        result.error = std::string(row_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto row = std::move(*row_result);
    if (!row.found)
    {
        (void)exec_sql("ROLLBACK");
        result.error = std::format("checkout: entity ({},{}) not found", type_id, dbid);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (row.checked_out_by.has_value())
    {
        (void)exec_sql("ROLLBACK");
        result.success = true;
        result.data = std::move(row.data);
        result.checked_out_by = row.checked_out_by;
        fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                      { cb(std::move(result)); });
        return;
    }

    auto update_result = prepare(
        "UPDATE entities SET checked_out = 1, checkout_ip = ?, checkout_port = ?, "
        "checkout_app_id = ?, checkout_eid = ?, updated_at_ms = ? "
        "WHERE dbid = ? AND type_id = ? AND checked_out = 0");
    if (!update_result)
    {
        (void)exec_sql("ROLLBACK");
        result.error = std::string(update_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto stmt = std::move(*update_result);
    auto rc = sqlite3_bind_int64(stmt.get(), 1, new_owner.base_addr.ip());
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 2, new_owner.base_addr.port());
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 3, static_cast<int>(new_owner.app_id));
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 4, static_cast<int>(new_owner.entity_id));
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 5, now_ms);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 6, dbid);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 7, static_cast<int>(type_id));

    if (rc != SQLITE_OK || sqlite3_step(stmt.get()) != SQLITE_DONE)
    {
        (void)exec_sql("ROLLBACK");
        result.error =
            std::string(sqlite_error("SqliteDatabase: checkout update failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (sqlite3_changes(db_) != 1)
    {
        (void)exec_sql("ROLLBACK");
        auto refreshed = fetch_by_dbid(dbid, type_id);
        if (refreshed && refreshed->found && refreshed->checked_out_by.has_value())
        {
            result.success = true;
            result.data = std::move(refreshed->data);
            result.checked_out_by = refreshed->checked_out_by;
        }
        else
        {
            result.error = "checkout conflict";
        }
        fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                      { cb(std::move(result)); });
        return;
    }

    auto commit_result = exec_sql("COMMIT");
    if (!commit_result)
    {
        (void)exec_sql("ROLLBACK");
        result.error = std::string(commit_result.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    result.success = true;
    result.data = std::move(row.data);
    fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                  { cb(std::move(result)); });
}

void SqliteDatabase::checkout_entity_by_name(uint16_t type_id, const std::string& identifier,
                                             const CheckoutInfo& new_owner,
                                             std::function<void(GetResult)> callback)
{
    auto lookup = fetch_by_name(type_id, identifier);
    if (!lookup)
    {
        GetResult result;
        result.error = std::string(lookup.error().message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (!lookup->found)
    {
        GetResult result;
        result.error = std::format("checkout_by_name: '{}' not found", identifier);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    checkout_entity(lookup->data.dbid, type_id, new_owner, std::move(callback));
}

void SqliteDatabase::clear_checkout(DatabaseID dbid, uint16_t type_id,
                                    std::function<void(bool)> callback)
{
    bool cleared = false;
    const auto now_ms = unix_time_ms();
    auto stmt_result = prepare(
        "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
        "WHERE dbid = ? AND type_id = ? AND checked_out = 1");
    if (stmt_result)
    {
        auto stmt = std::move(*stmt_result);
        auto rc = sqlite3_bind_int64(stmt.get(), 1, now_ms);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(stmt.get(), 2, dbid);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 3, static_cast<int>(type_id));
        if (rc == SQLITE_OK && sqlite3_step(stmt.get()) == SQLITE_DONE)
        {
            cleared = sqlite3_changes(db_) > 0;
        }
    }

    fire_or_defer([cb = std::move(callback), cleared]() mutable { cb(cleared); });
}

void SqliteDatabase::clear_checkouts_for_address(const Address& base_addr,
                                                 std::function<void(int cleared_count)> callback)
{
    int cleared = 0;
    const auto now_ms = unix_time_ms();
    auto stmt_result = prepare(
        "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
        "WHERE checked_out = 1 AND checkout_ip = ? AND checkout_port = ?");
    if (stmt_result)
    {
        auto stmt = std::move(*stmt_result);
        auto rc = sqlite3_bind_int64(stmt.get(), 1, now_ms);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int64(stmt.get(), 2, base_addr.ip());
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_int(stmt.get(), 3, base_addr.port());
        if (rc == SQLITE_OK && sqlite3_step(stmt.get()) == SQLITE_DONE)
        {
            cleared = sqlite3_changes(db_);
        }
    }

    fire_or_defer([cb = std::move(callback), cleared]() mutable { cb(cleared); });
}

void SqliteDatabase::mark_checkout_cleared(DatabaseID dbid, uint16_t type_id)
{
    const auto now_ms = unix_time_ms();
    auto stmt_result = prepare(
        "UPDATE entities SET checked_out = 0, checkout_ip = 0, checkout_port = 0, "
        "checkout_app_id = 0, checkout_eid = 0, updated_at_ms = ? "
        "WHERE dbid = ? AND type_id = ? AND checked_out = 1");
    if (!stmt_result)
    {
        return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.get(), 1, now_ms);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 2, dbid);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 3, static_cast<int>(type_id));
    if (rc == SQLITE_OK)
    {
        (void)sqlite3_step(stmt.get());
    }
}

void SqliteDatabase::get_auto_load_entities(std::function<void(std::vector<EntityData>)> callback)
{
    std::vector<EntityData> result;
    auto stmt_result = prepare(
        "SELECT dbid, type_id, blob, identifier FROM entities WHERE auto_load = 1 "
        "ORDER BY type_id, dbid");
    if (!stmt_result)
    {
        fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                      { cb(std::move(result)); });
        return;
    }

    auto stmt = std::move(*stmt_result);
    for (;;)
    {
        auto rc = sqlite3_step(stmt.get());
        if (rc == SQLITE_DONE)
        {
            break;
        }
        if (rc != SQLITE_ROW)
        {
            break;
        }

        EntityData data;
        data.dbid = static_cast<DatabaseID>(sqlite3_column_int64(stmt.get(), 0));
        data.type_id = static_cast<uint16_t>(sqlite3_column_int(stmt.get(), 1));
        auto blob_ptr = sqlite3_column_blob(stmt.get(), 2);
        auto blob_size = sqlite3_column_bytes(stmt.get(), 2);
        if (blob_ptr != nullptr && blob_size > 0)
        {
            auto* bytes = static_cast<const std::byte*>(blob_ptr);
            data.blob.assign(bytes, bytes + blob_size);
        }
        if (const auto* text = sqlite3_column_text(stmt.get(), 3); text != nullptr)
        {
            data.identifier = reinterpret_cast<const char*>(text);
        }
        result.push_back(std::move(data));
    }

    fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                  { cb(std::move(result)); });
}

void SqliteDatabase::set_auto_load(DatabaseID dbid, uint16_t type_id, bool auto_load)
{
    const auto now_ms = unix_time_ms();
    auto stmt_result = prepare(
        "UPDATE entities SET auto_load = ?, updated_at_ms = ? WHERE dbid = ? AND type_id = ?");
    if (!stmt_result)
    {
        return;
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int(stmt.get(), 1, auto_load ? 1 : 0);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 2, now_ms);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int64(stmt.get(), 3, dbid);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 4, static_cast<int>(type_id));
    if (rc == SQLITE_OK)
    {
        (void)sqlite3_step(stmt.get());
    }
}

void SqliteDatabase::process_results()
{
    if (!deferred_mode_)
    {
        return;
    }

    int budget = kMaxCallbacksPerTick;
    while (!deferred_.empty() && budget-- > 0)
    {
        auto cb = std::move(deferred_.front());
        deferred_.pop_front();
        cb();
    }
}

auto SqliteDatabase::open_database(const DatabaseConfig& config) -> Result<void>
{
    auto parent = db_path_.parent_path();
    if (!parent.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec)
        {
            return Error{ErrorCode::IoError,
                         std::format("SqliteDatabase: cannot create dir '{}': {}", parent.string(),
                                     ec.message())};
        }
    }

    auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    auto rc = sqlite3_open_v2(db_path_.string().c_str(), &db_, flags, nullptr);
    if (rc != SQLITE_OK || db_ == nullptr)
    {
        return sqlite_error("SqliteDatabase: sqlite3_open_v2 failed", rc);
    }

    rc = sqlite3_busy_timeout(db_, config.sqlite_busy_timeout_ms);
    if (rc != SQLITE_OK)
    {
        return sqlite_error("SqliteDatabase: sqlite3_busy_timeout failed", rc);
    }

    auto journal_sql = std::string(config.sqlite_wal ? "PRAGMA journal_mode = WAL"
                                                     : "PRAGMA journal_mode = DELETE");
    auto pragma_result = exec_sql(journal_sql);
    if (!pragma_result)
    {
        return pragma_result.error();
    }

    auto fk_result = exec_sql(std::string(
        config.sqlite_foreign_keys ? "PRAGMA foreign_keys = ON" : "PRAGMA foreign_keys = OFF"));
    if (!fk_result)
    {
        return fk_result.error();
    }

    auto sync_result = exec_sql("PRAGMA synchronous = NORMAL");
    if (!sync_result)
    {
        return sync_result.error();
    }

    return {};
}

auto SqliteDatabase::ensure_schema() -> Result<void>
{
    auto schema_result = exec_sql(
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
    if (!schema_result)
    {
        return schema_result.error();
    }

    auto created_at_result = exec_sql_ignoring_duplicate_column(
        "ALTER TABLE entities ADD COLUMN created_at_ms INTEGER NOT NULL DEFAULT 0");
    if (!created_at_result)
    {
        return created_at_result.error();
    }

    auto updated_at_result = exec_sql_ignoring_duplicate_column(
        "ALTER TABLE entities ADD COLUMN updated_at_ms INTEGER NOT NULL DEFAULT 0");
    if (!updated_at_result)
    {
        return updated_at_result.error();
    }

    auto meta_table_result =
        exec_sql("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    if (!meta_table_result)
    {
        return meta_table_result.error();
    }

    auto schema_version_result =
        upsert_meta("schema_version", std::to_string(kSqliteSchemaVersion));
    if (!schema_version_result)
    {
        return schema_version_result.error();
    }

    auto index1 =
        exec_sql("CREATE INDEX IF NOT EXISTS idx_entities_type_dbid ON entities(type_id, dbid)");
    if (!index1)
    {
        return index1.error();
    }

    auto index2 = exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_entities_type_identifier "
        "ON entities(type_id, identifier)");
    if (!index2)
    {
        return index2.error();
    }

    auto index3 = exec_sql(
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_entities_type_identifier_unique "
        "ON entities(type_id, identifier) "
        "WHERE identifier IS NOT NULL AND identifier <> ''");
    if (!index3)
    {
        return index3.error();
    }

    auto index4 = exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_entities_auto_load ON entities(auto_load, type_id, dbid)");
    if (!index4)
    {
        return index4.error();
    }

    auto index5 = exec_sql(
        "CREATE INDEX IF NOT EXISTS idx_entities_checkout_addr "
        "ON entities(checked_out, checkout_ip, checkout_port)");
    if (!index5)
    {
        return index5.error();
    }

    return {};
}

auto SqliteDatabase::exec_sql(std::string_view sql) -> Result<void>
{
    char* err = nullptr;
    auto rc = sqlite3_exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err);
    if (rc == SQLITE_OK)
    {
        return {};
    }

    std::string message = std::format("SqliteDatabase: exec failed '{}'", sql);
    if (err != nullptr)
    {
        message += ": ";
        message += err;
        sqlite3_free(err);
    }
    else if (db_ != nullptr)
    {
        message += ": ";
        message += sqlite3_errmsg(db_);
    }

    return Error{ErrorCode::InternalError, std::move(message)};
}

auto SqliteDatabase::exec_sql_ignoring_duplicate_column(std::string_view sql) -> Result<void>
{
    auto result = exec_sql(sql);
    if (result)
    {
        return result;
    }

    if (std::string_view(result.error().message()).find("duplicate column name") !=
        std::string_view::npos)
    {
        return {};
    }

    return result.error();
}

auto SqliteDatabase::prepare(std::string_view sql) -> Result<Statement>
{
    sqlite3_stmt* stmt = nullptr;
    auto rc = sqlite3_prepare_v2(db_, std::string(sql).c_str(), -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        return sqlite_error(std::format("SqliteDatabase: prepare failed '{}'", sql), rc);
    }
    return Statement{stmt};
}

auto SqliteDatabase::upsert_meta(std::string_view key, std::string_view value) -> Result<void>
{
    auto stmt_result = prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    if (!stmt_result)
    {
        return stmt_result.error();
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_text(stmt.get(), 1, std::string(key).c_str(), -1, SQLITE_TRANSIENT);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt.get(), 2, std::string(value).c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        return sqlite_error("SqliteDatabase: meta bind failed", rc);
    }

    rc = sqlite3_step(stmt.get());
    if (rc != SQLITE_DONE)
    {
        return sqlite_error("SqliteDatabase: meta upsert failed", rc);
    }

    return {};
}

auto SqliteDatabase::fetch_by_dbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow>
{
    auto stmt_result = prepare(
        "SELECT dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
        "checkout_ip, checkout_port, checkout_app_id, checkout_eid "
        "FROM entities WHERE dbid = ? AND type_id = ? LIMIT 1");
    if (!stmt_result)
    {
        return stmt_result.error();
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int64(stmt.get(), 1, dbid);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_int(stmt.get(), 2, static_cast<int>(type_id));
    if (rc != SQLITE_OK)
    {
        return sqlite_error("SqliteDatabase: fetch_by_dbid bind failed", rc);
    }

    rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
    {
        return EntityRow{};
    }
    if (rc != SQLITE_ROW)
    {
        return sqlite_error("SqliteDatabase: fetch_by_dbid step failed", rc);
    }
    return read_row(stmt.get());
}

auto SqliteDatabase::fetch_by_name(uint16_t type_id, std::string_view identifier)
    -> Result<EntityRow>
{
    auto stmt_result = prepare(
        "SELECT dbid, type_id, blob, identifier, password_hash, auto_load, checked_out, "
        "checkout_ip, checkout_port, checkout_app_id, checkout_eid "
        "FROM entities WHERE type_id = ? AND identifier = ? LIMIT 1");
    if (!stmt_result)
    {
        return stmt_result.error();
    }

    auto stmt = std::move(*stmt_result);
    auto rc = sqlite3_bind_int(stmt.get(), 1, static_cast<int>(type_id));
    auto owned_identifier = std::string(identifier);
    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(stmt.get(), 2, owned_identifier.c_str(), -1, SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        return sqlite_error("SqliteDatabase: fetch_by_name bind failed", rc);
    }

    rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE)
    {
        return EntityRow{};
    }
    if (rc != SQLITE_ROW)
    {
        return sqlite_error("SqliteDatabase: fetch_by_name step failed", rc);
    }
    return read_row(stmt.get());
}

auto SqliteDatabase::read_row(sqlite3_stmt* stmt) const -> EntityRow
{
    EntityRow row;
    row.found = true;
    row.data.dbid = static_cast<DatabaseID>(sqlite3_column_int64(stmt, 0));
    row.data.type_id = static_cast<uint16_t>(sqlite3_column_int(stmt, 1));

    auto blob_ptr = sqlite3_column_blob(stmt, 2);
    auto blob_size = sqlite3_column_bytes(stmt, 2);
    if (blob_ptr != nullptr && blob_size > 0)
    {
        auto* bytes = static_cast<const std::byte*>(blob_ptr);
        row.data.blob.assign(bytes, bytes + blob_size);
    }

    if (const auto* text = sqlite3_column_text(stmt, 3); text != nullptr)
    {
        row.data.identifier = reinterpret_cast<const char*>(text);
    }
    if (const auto* text = sqlite3_column_text(stmt, 4); text != nullptr)
    {
        row.password_hash = reinterpret_cast<const char*>(text);
    }

    row.auto_load = sqlite3_column_int(stmt, 5) != 0;
    const bool checked_out = sqlite3_column_int(stmt, 6) != 0;
    if (checked_out)
    {
        CheckoutInfo info;
        info.base_addr = Address(static_cast<uint32_t>(sqlite3_column_int64(stmt, 7)),
                                 static_cast<uint16_t>(sqlite3_column_int(stmt, 8)));
        info.app_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 9));
        info.entity_id = static_cast<uint32_t>(sqlite3_column_int(stmt, 10));
        row.checked_out_by = info;
    }

    return row;
}

auto SqliteDatabase::bind_identifier(Statement& stmt, int index,
                                     const std::string& identifier) const -> Result<void>
{
    int rc = SQLITE_OK;
    if (identifier.empty())
    {
        rc = sqlite3_bind_null(stmt.get(), index);
    }
    else
    {
        rc = sqlite3_bind_text(stmt.get(), index, identifier.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (rc != SQLITE_OK)
    {
        return sqlite_error("SqliteDatabase: bind identifier failed", rc);
    }
    return {};
}

auto SqliteDatabase::bind_blob(Statement& stmt, int index, std::span<const std::byte> blob) const
    -> Result<void>
{
    static const std::byte kEmptyBlob = std::byte{0};
    const void* data = blob.empty() ? &kEmptyBlob : blob.data();
    auto rc =
        sqlite3_bind_blob(stmt.get(), index, data, static_cast<int>(blob.size()), SQLITE_TRANSIENT);
    if (rc != SQLITE_OK)
    {
        return sqlite_error("SqliteDatabase: bind blob failed", rc);
    }
    return {};
}

auto SqliteDatabase::sqlite_error(std::string_view prefix) const -> Error
{
    std::string message(prefix);
    if (db_ != nullptr)
    {
        message += ": ";
        message += sqlite3_errmsg(db_);
    }
    return Error{ErrorCode::InternalError, std::move(message)};
}

auto SqliteDatabase::sqlite_error(std::string_view prefix, int code) const -> Error
{
    return Error{ErrorCode::InternalError,
                 std::format("{} (sqlite rc={}){}", prefix, code,
                             db_ != nullptr ? std::format(": {}", sqlite3_errmsg(db_)) : "")};
}

void SqliteDatabase::fire_or_defer(std::function<void()> cb)
{
    if (deferred_mode_)
    {
        deferred_.push_back(std::move(cb));
    }
    else
    {
        cb();
    }
}

}  // namespace atlas
