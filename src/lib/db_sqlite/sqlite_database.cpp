#include "db_sqlite/sqlite_database.hpp"

#include "foundation/log.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <string_view>

namespace
{

auto sqlite_transient() -> void (*)(void*)
{
    return reinterpret_cast<void (*)(void*)>(static_cast<intptr_t>(-1));
}

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

SqliteDatabase::Statement::Statement(Statement&& other) noexcept
    : api_(other.api_), stmt_(other.stmt_)
{
    other.api_ = nullptr;
    other.stmt_ = nullptr;
}

auto SqliteDatabase::Statement::operator=(Statement&& other) noexcept -> Statement&
{
    if (this == &other)
    {
        return *this;
    }
    reset();
    api_ = other.api_;
    stmt_ = other.stmt_;
    other.api_ = nullptr;
    other.stmt_ = nullptr;
    return *this;
}

SqliteDatabase::Statement::~Statement()
{
    reset();
}

void SqliteDatabase::Statement::reset()
{
    if (stmt_ != nullptr && api_ != nullptr)
    {
        (void)api_->finalize(stmt_);
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

    auto api_result = load_sqlite_api();
    if (!api_result)
    {
        return api_result.error();
    }
    api_ = std::move(*api_result);

    auto open_result = open_database(config);
    if (!open_result)
    {
        api_.reset();
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
    if (db_ != nullptr && api_.has_value())
    {
        (void)api_->close(db_);
        db_ = nullptr;
    }
    deferred_.clear();
    started_ = false;
    api_.reset();
}

void SqliteDatabase::put_entity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                std::span<const std::byte> blob, const std::string& identifier,
                                std::function<void(PutResult)> callback)
{
    PutResult result;

    if (!started_ || db_ == nullptr || !api_.has_value())
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
        auto rc = api_->bind_int(stmt.get(), 1, static_cast<int>(type_id));
        if (rc == kSqliteOk)
        {
            auto bind_result = bind_blob(stmt, 2, blob);
            rc = bind_result ? kSqliteOk : 1;
            if (!bind_result)
                result.error = std::string(bind_result.error().message());
        }
        if (rc == kSqliteOk)
        {
            auto bind_result = bind_identifier(stmt, 3, identifier);
            rc = bind_result ? kSqliteOk : 1;
            if (!bind_result)
                result.error = std::string(bind_result.error().message());
        }
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 4, has_flag(flags, WriteFlags::AutoLoadOn) ? 1 : 0);
        if (rc == kSqliteOk)
            rc = api_->bind_int64(stmt.get(), 5, now_ms);
        if (rc == kSqliteOk)
            rc = api_->bind_int64(stmt.get(), 6, now_ms);

        if (rc != kSqliteOk)
        {
            if (result.error.empty())
                result.error =
                    std::string(sqlite_error("SqliteDatabase: insert bind failed").message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        rc = api_->step(stmt.get());
        if (rc != kSqliteDone)
        {
            result.error = std::string(sqlite_error("SqliteDatabase: insert failed", rc).message());
            fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
            return;
        }

        result.success = true;
        result.dbid = static_cast<DatabaseID>(api_->last_insert_rowid(db_));
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
    auto rc = kSqliteOk;
    auto bind_blob_result = bind_blob(stmt, 1, blob);
    rc = bind_blob_result ? kSqliteOk : 1;
    if (!bind_blob_result)
        result.error = std::string(bind_blob_result.error().message());
    if (rc == kSqliteOk)
    {
        auto bind_result = bind_identifier(stmt, 2, final_identifier);
        rc = bind_result ? kSqliteOk : 1;
        if (!bind_result)
            result.error = std::string(bind_result.error().message());
    }
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 3, final_auto_load ? 1 : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 4, final_checked_out ? 1 : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 5, final_checked_out ? final_owner.base_addr.ip() : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 6, final_checked_out ? final_owner.base_addr.port() : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 7, final_checked_out ? final_owner.app_id : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 8, final_checked_out ? final_owner.entity_id : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 9, now_ms);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 10, dbid);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 11, static_cast<int>(type_id));

    if (rc != kSqliteOk)
    {
        if (result.error.empty())
            result.error =
                std::string(sqlite_error("SqliteDatabase: update bind failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    rc = api_->step(stmt.get());
    if (rc != kSqliteDone)
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

    if (!started_ || db_ == nullptr || !api_.has_value())
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
        auto rc = kSqliteOk;
        auto bind_blob_result = bind_blob(stmt, 1, blob);
        rc = bind_blob_result ? kSqliteOk : 1;
        if (!bind_blob_result)
            result.error = std::string(bind_blob_result.error().message());
        if (rc == kSqliteOk)
        {
            auto bind_identifier_result = bind_identifier(stmt, 2, final_identifier);
            rc = bind_identifier_result ? kSqliteOk : 1;
            if (!bind_identifier_result)
                result.error = std::string(bind_identifier_result.error().message());
        }
        if (rc == kSqliteOk)
        {
            if (password_hash.empty())
                rc = api_->bind_null(stmt.get(), 3);
            else
                rc = api_->bind_text(stmt.get(), 3, password_hash.c_str(), -1, sqlite_transient());
        }
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 4, final_auto_load ? 1 : 0);
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 5, final_checked_out ? 1 : 0);
        if (rc == kSqliteOk)
            rc =
                api_->bind_int64(stmt.get(), 6, final_checked_out ? final_owner.base_addr.ip() : 0);
        if (rc == kSqliteOk)
            rc =
                api_->bind_int(stmt.get(), 7, final_checked_out ? final_owner.base_addr.port() : 0);
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 8, final_checked_out ? final_owner.app_id : 0);
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 9, final_checked_out ? final_owner.entity_id : 0);
        if (rc == kSqliteOk)
            rc = api_->bind_int64(stmt.get(), 10, now_ms);
        if (rc == kSqliteOk)
            rc = api_->bind_int64(stmt.get(), 11, dbid);
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 12, static_cast<int>(type_id));

        if (rc != kSqliteOk || api_->step(stmt.get()) != kSqliteDone)
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
    auto rc = api_->bind_int(stmt.get(), 1, static_cast<int>(type_id));
    if (rc == kSqliteOk)
    {
        auto bind_result = bind_blob(stmt, 2, blob);
        rc = bind_result ? kSqliteOk : 1;
        if (!bind_result)
            result.error = std::string(bind_result.error().message());
    }
    if (rc == kSqliteOk)
    {
        auto bind_result = bind_identifier(stmt, 3, identifier);
        rc = bind_result ? kSqliteOk : 1;
        if (!bind_result)
            result.error = std::string(bind_result.error().message());
    }
    if (rc == kSqliteOk)
    {
        if (password_hash.empty())
            rc = api_->bind_null(stmt.get(), 4);
        else
            rc = api_->bind_text(stmt.get(), 4, password_hash.c_str(), -1, sqlite_transient());
    }
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 5, has_flag(flags, WriteFlags::AutoLoadOn) ? 1 : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 6, now_ms);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 7, now_ms);

    if (rc != kSqliteOk)
    {
        if (result.error.empty())
            result.error =
                std::string(sqlite_error("SqliteDatabase: password insert bind failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    rc = api_->step(stmt.get());
    if (rc != kSqliteDone)
    {
        result.error =
            std::string(sqlite_error("SqliteDatabase: password insert failed", rc).message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    result.success = true;
    result.dbid = static_cast<DatabaseID>(api_->last_insert_rowid(db_));
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
    auto rc = api_->bind_int64(stmt.get(), 1, dbid);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 2, static_cast<int>(type_id));
    if (rc != kSqliteOk)
    {
        result.error = std::string(sqlite_error("SqliteDatabase: delete bind failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    rc = api_->step(stmt.get());
    if (rc != kSqliteDone)
    {
        result.error = std::string(sqlite_error("SqliteDatabase: delete failed", rc).message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (api_->changes(db_) <= 0)
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
    auto rc = api_->bind_int64(stmt.get(), 1, new_owner.base_addr.ip());
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 2, new_owner.base_addr.port());
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 3, static_cast<int>(new_owner.app_id));
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 4, static_cast<int>(new_owner.entity_id));
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 5, now_ms);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 6, dbid);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 7, static_cast<int>(type_id));

    if (rc != kSqliteOk || api_->step(stmt.get()) != kSqliteDone)
    {
        (void)exec_sql("ROLLBACK");
        result.error =
            std::string(sqlite_error("SqliteDatabase: checkout update failed").message());
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (api_->changes(db_) != 1)
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
        auto rc = api_->bind_int64(stmt.get(), 1, now_ms);
        if (rc == kSqliteOk)
            rc = api_->bind_int64(stmt.get(), 2, dbid);
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 3, static_cast<int>(type_id));
        if (rc == kSqliteOk && api_->step(stmt.get()) == kSqliteDone)
        {
            cleared = api_->changes(db_) > 0;
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
        auto rc = api_->bind_int64(stmt.get(), 1, now_ms);
        if (rc == kSqliteOk)
            rc = api_->bind_int64(stmt.get(), 2, base_addr.ip());
        if (rc == kSqliteOk)
            rc = api_->bind_int(stmt.get(), 3, base_addr.port());
        if (rc == kSqliteOk && api_->step(stmt.get()) == kSqliteDone)
        {
            cleared = api_->changes(db_);
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
    auto rc = api_->bind_int64(stmt.get(), 1, now_ms);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 2, dbid);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 3, static_cast<int>(type_id));
    if (rc == kSqliteOk)
    {
        (void)api_->step(stmt.get());
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
        auto rc = api_->step(stmt.get());
        if (rc == kSqliteDone)
        {
            break;
        }
        if (rc != kSqliteRow)
        {
            break;
        }

        EntityData data;
        data.dbid = static_cast<DatabaseID>(api_->column_int64(stmt.get(), 0));
        data.type_id = static_cast<uint16_t>(api_->column_int(stmt.get(), 1));
        auto blob_ptr = api_->column_blob(stmt.get(), 2);
        auto blob_size = api_->column_bytes(stmt.get(), 2);
        if (blob_ptr != nullptr && blob_size > 0)
        {
            auto* bytes = static_cast<const std::byte*>(blob_ptr);
            data.blob.assign(bytes, bytes + blob_size);
        }
        if (const auto* text = api_->column_text(stmt.get(), 3); text != nullptr)
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
    auto rc = api_->bind_int(stmt.get(), 1, auto_load ? 1 : 0);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 2, now_ms);
    if (rc == kSqliteOk)
        rc = api_->bind_int64(stmt.get(), 3, dbid);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 4, static_cast<int>(type_id));
    if (rc == kSqliteOk)
    {
        (void)api_->step(stmt.get());
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

auto SqliteDatabase::load_sqlite_api() -> Result<SqliteApi>
{
    static constexpr std::string_view kCandidates[] = {
#if defined(_WIN32)
        "sqlite3.dll",
        "winsqlite3.dll",
#elif defined(__APPLE__)
        "libsqlite3.dylib",
        "/usr/lib/libsqlite3.dylib",
#else
        "libsqlite3.so.0",
        "libsqlite3.so",
        "/usr/lib/x86_64-linux-gnu/libsqlite3.so.0",
#endif
    };

    Error last_error{ErrorCode::NotFound, "sqlite runtime library not found"};
    for (auto candidate : kCandidates)
    {
        std::cout << std::format("dynamic load sqlite dll: {}\n", candidate);

        auto lib_result = DynamicLibrary::load(std::filesystem::path(candidate));
        if (!lib_result)
        {
            last_error = lib_result.error();
            continue;
        }

        SqliteApi api{std::move(*lib_result)};

        auto load_symbol = [&]<typename Fn>(Fn& out, std::string_view name) -> bool
        {
            std::cout << std::format("sqlite load symbol name: {}\n", name);

            auto sym = api.library.get_symbol<Fn>(name);
            if (!sym)
            {
                last_error = sym.error();
                std::cout << std::format("> load symbol {} failed.\n", name);
                return false;
            }
            out = *sym;
            return true;
        };

        std::cout << "sqlite load symbol check \n";

        if (!load_symbol(api.open_v2, "sqlite3_open_v2") ||
            !load_symbol(api.close, "sqlite3_close") || !load_symbol(api.exec, "sqlite3_exec") ||
            !load_symbol(api.free_fn, "sqlite3_free") ||
            !load_symbol(api.errmsg, "sqlite3_errmsg") ||
            !load_symbol(api.busy_timeout, "sqlite3_busy_timeout") ||
            !load_symbol(api.prepare_v2, "sqlite3_prepare_v2") ||
            !load_symbol(api.finalize, "sqlite3_finalize") ||
            !load_symbol(api.step, "sqlite3_step") ||
            !load_symbol(api.bind_int, "sqlite3_bind_int") ||
            !load_symbol(api.bind_int64, "sqlite3_bind_int64") ||
            !load_symbol(api.bind_null, "sqlite3_bind_null") ||
            !load_symbol(api.bind_text, "sqlite3_bind_text") ||
            !load_symbol(api.bind_blob, "sqlite3_bind_blob") ||
            !load_symbol(api.column_int, "sqlite3_column_int") ||
            !load_symbol(api.column_int64, "sqlite3_column_int64") ||
            !load_symbol(api.column_text, "sqlite3_column_text") ||
            !load_symbol(api.column_blob, "sqlite3_column_blob") ||
            !load_symbol(api.column_bytes, "sqlite3_column_bytes") ||
            !load_symbol(api.last_insert_rowid, "sqlite3_last_insert_rowid") ||
            !load_symbol(api.changes, "sqlite3_changes"))
        {
            continue;
        }

        std::cout << "load sqlite api succeed\n";
        return api;
    }

    return Error{last_error.code(), std::string("failed to load sqlite runtime: ") +
                                        std::string(last_error.message())};
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

    auto flags = kSqliteOpenReadWrite | kSqliteOpenCreate | kSqliteOpenFullMutex;
    auto rc = api_->open_v2(db_path_.string().c_str(), &db_, flags, nullptr);
    if (rc != kSqliteOk || db_ == nullptr)
    {
        return sqlite_error("SqliteDatabase: sqlite3_open_v2 failed", rc);
    }

    rc = api_->busy_timeout(db_, config.sqlite_busy_timeout_ms);
    if (rc != kSqliteOk)
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
    auto rc = api_->exec(db_, std::string(sql).c_str(), nullptr, nullptr, &err);
    if (rc == kSqliteOk)
    {
        return {};
    }

    std::string message = std::format("SqliteDatabase: exec failed '{}'", sql);
    if (err != nullptr)
    {
        message += ": ";
        message += err;
        api_->free_fn(err);
    }
    else if (db_ != nullptr)
    {
        message += ": ";
        message += api_->errmsg(db_);
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
    auto rc = api_->prepare_v2(db_, std::string(sql).c_str(), -1, &stmt, nullptr);
    if (rc != kSqliteOk)
    {
        return sqlite_error(std::format("SqliteDatabase: prepare failed '{}'", sql), rc);
    }
    return Statement{&*api_, stmt};
}

auto SqliteDatabase::upsert_meta(std::string_view key, std::string_view value) -> Result<void>
{
    auto stmt_result = prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    if (!stmt_result)
    {
        return stmt_result.error();
    }

    auto stmt = std::move(*stmt_result);
    auto rc = api_->bind_text(stmt.get(), 1, std::string(key).c_str(), -1, sqlite_transient());
    if (rc == kSqliteOk)
        rc = api_->bind_text(stmt.get(), 2, std::string(value).c_str(), -1, sqlite_transient());
    if (rc != kSqliteOk)
    {
        return sqlite_error("SqliteDatabase: meta bind failed", rc);
    }

    rc = api_->step(stmt.get());
    if (rc != kSqliteDone)
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
    auto rc = api_->bind_int64(stmt.get(), 1, dbid);
    if (rc == kSqliteOk)
        rc = api_->bind_int(stmt.get(), 2, static_cast<int>(type_id));
    if (rc != kSqliteOk)
    {
        return sqlite_error("SqliteDatabase: fetch_by_dbid bind failed", rc);
    }

    rc = api_->step(stmt.get());
    if (rc == kSqliteDone)
    {
        return EntityRow{};
    }
    if (rc != kSqliteRow)
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
    auto rc = api_->bind_int(stmt.get(), 1, static_cast<int>(type_id));
    auto owned_identifier = std::string(identifier);
    if (rc == kSqliteOk)
        rc = api_->bind_text(stmt.get(), 2, owned_identifier.c_str(), -1, sqlite_transient());
    if (rc != kSqliteOk)
    {
        return sqlite_error("SqliteDatabase: fetch_by_name bind failed", rc);
    }

    rc = api_->step(stmt.get());
    if (rc == kSqliteDone)
    {
        return EntityRow{};
    }
    if (rc != kSqliteRow)
    {
        return sqlite_error("SqliteDatabase: fetch_by_name step failed", rc);
    }
    return read_row(stmt.get());
}

auto SqliteDatabase::read_row(sqlite3_stmt* stmt) const -> EntityRow
{
    EntityRow row;
    row.found = true;
    row.data.dbid = static_cast<DatabaseID>(api_->column_int64(stmt, 0));
    row.data.type_id = static_cast<uint16_t>(api_->column_int(stmt, 1));

    auto blob_ptr = api_->column_blob(stmt, 2);
    auto blob_size = api_->column_bytes(stmt, 2);
    if (blob_ptr != nullptr && blob_size > 0)
    {
        auto* bytes = static_cast<const std::byte*>(blob_ptr);
        row.data.blob.assign(bytes, bytes + blob_size);
    }

    if (const auto* text = api_->column_text(stmt, 3); text != nullptr)
    {
        row.data.identifier = reinterpret_cast<const char*>(text);
    }
    if (const auto* text = api_->column_text(stmt, 4); text != nullptr)
    {
        row.password_hash = reinterpret_cast<const char*>(text);
    }

    row.auto_load = api_->column_int(stmt, 5) != 0;
    const bool checked_out = api_->column_int(stmt, 6) != 0;
    if (checked_out)
    {
        CheckoutInfo info;
        info.base_addr = Address(static_cast<uint32_t>(api_->column_int64(stmt, 7)),
                                 static_cast<uint16_t>(api_->column_int(stmt, 8)));
        info.app_id = static_cast<uint32_t>(api_->column_int(stmt, 9));
        info.entity_id = static_cast<uint32_t>(api_->column_int(stmt, 10));
        row.checked_out_by = info;
    }

    return row;
}

auto SqliteDatabase::bind_identifier(Statement& stmt, int index,
                                     const std::string& identifier) const -> Result<void>
{
    int rc = kSqliteOk;
    if (identifier.empty())
    {
        rc = api_->bind_null(stmt.get(), index);
    }
    else
    {
        rc = api_->bind_text(stmt.get(), index, identifier.c_str(), -1, sqlite_transient());
    }
    if (rc != kSqliteOk)
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
        api_->bind_blob(stmt.get(), index, data, static_cast<int>(blob.size()), sqlite_transient());
    if (rc != kSqliteOk)
    {
        return sqlite_error("SqliteDatabase: bind blob failed", rc);
    }
    return {};
}

auto SqliteDatabase::sqlite_error(std::string_view prefix) const -> Error
{
    std::string message(prefix);
    if (db_ != nullptr && api_.has_value() && api_->errmsg != nullptr)
    {
        message += ": ";
        message += api_->errmsg(db_);
    }
    return Error{ErrorCode::InternalError, std::move(message)};
}

auto SqliteDatabase::sqlite_error(std::string_view prefix, int code) const -> Error
{
    return Error{ErrorCode::InternalError,
                 std::format("{} (sqlite rc={}){}", prefix, code,
                             (db_ != nullptr && api_.has_value() && api_->errmsg != nullptr)
                                 ? std::format(": {}", api_->errmsg(db_))
                                 : "")};
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
