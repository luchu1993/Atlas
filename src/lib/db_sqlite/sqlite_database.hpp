#pragma once

#include "db/idatabase.hpp"

#include <deque>
#include <functional>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <vector>

namespace atlas
{

class SqliteDatabase : public IDatabase
{
public:
    SqliteDatabase() = default;
    ~SqliteDatabase() override;

    void set_deferred_mode(bool enabled) override { deferred_mode_ = enabled; }

    [[nodiscard]] auto startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
        -> Result<void> override;
    void shutdown() override;

    void put_entity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                    std::span<const std::byte> blob, const std::string& identifier,
                    std::function<void(PutResult)> callback) override;

    void put_entity_with_password(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                  std::span<const std::byte> blob, const std::string& identifier,
                                  const std::string& password_hash,
                                  std::function<void(PutResult)> callback) override;

    void get_entity(DatabaseID dbid, uint16_t type_id,
                    std::function<void(GetResult)> callback) override;

    void del_entity(DatabaseID dbid, uint16_t type_id,
                    std::function<void(DelResult)> callback) override;

    void lookup_by_name(uint16_t type_id, const std::string& identifier,
                        std::function<void(LookupResult)> callback) override;

    void checkout_entity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                         std::function<void(GetResult)> callback) override;

    void checkout_entity_by_name(uint16_t type_id, const std::string& identifier,
                                 const CheckoutInfo& new_owner,
                                 std::function<void(GetResult)> callback) override;

    void clear_checkout(DatabaseID dbid, uint16_t type_id,
                        std::function<void(bool)> callback) override;

    void clear_checkouts_for_address(const Address& base_addr,
                                     std::function<void(int cleared_count)> callback) override;

    void mark_checkout_cleared(DatabaseID dbid, uint16_t type_id) override;

    void get_auto_load_entities(std::function<void(std::vector<EntityData>)> callback) override;

    void set_auto_load(DatabaseID dbid, uint16_t type_id, bool auto_load) override;

    void process_results() override;

private:
    struct EntityRow
    {
        bool found{false};
        EntityData data;
        std::string password_hash;
        bool auto_load{false};
        std::optional<CheckoutInfo> checked_out_by;
    };

    class Statement
    {
    public:
        Statement() = default;
        explicit Statement(sqlite3_stmt* stmt) : stmt_(stmt) {}
        Statement(const Statement&) = delete;
        auto operator=(const Statement&) -> Statement& = delete;
        Statement(Statement&& other) noexcept;
        auto operator=(Statement&& other) noexcept -> Statement&;
        ~Statement();

        [[nodiscard]] auto get() const -> sqlite3_stmt* { return stmt_; }
        void reset();

    private:
        sqlite3_stmt* stmt_{nullptr};
    };

    [[nodiscard]] auto open_database(const DatabaseConfig& config) -> Result<void>;
    [[nodiscard]] auto ensure_schema() -> Result<void>;
    [[nodiscard]] auto exec_sql(std::string_view sql) -> Result<void>;
    [[nodiscard]] auto exec_sql_ignoring_duplicate_column(std::string_view sql) -> Result<void>;
    [[nodiscard]] auto prepare(std::string_view sql) -> Result<Statement>;
    [[nodiscard]] auto upsert_meta(std::string_view key, std::string_view value) -> Result<void>;
    [[nodiscard]] auto fetch_by_dbid(DatabaseID dbid, uint16_t type_id) -> Result<EntityRow>;
    [[nodiscard]] auto fetch_by_name(uint16_t type_id, std::string_view identifier)
        -> Result<EntityRow>;
    [[nodiscard]] auto read_row(sqlite3_stmt* stmt) const -> EntityRow;
    [[nodiscard]] auto bind_identifier(Statement& stmt, int index,
                                       const std::string& identifier) const -> Result<void>;
    [[nodiscard]] auto bind_blob(Statement& stmt, int index, std::span<const std::byte> blob) const
        -> Result<void>;
    [[nodiscard]] auto sqlite_error(std::string_view prefix) const -> Error;
    [[nodiscard]] auto sqlite_error(std::string_view prefix, int code) const -> Error;
    void fire_or_defer(std::function<void()> cb);

    sqlite3* db_{nullptr};
    const EntityDefRegistry* entity_defs_{nullptr};
    std::filesystem::path db_path_;
    bool started_{false};
    bool deferred_mode_{false};
    std::deque<std::function<void()>> deferred_;

    static constexpr int kMaxCallbacksPerTick = 2048;
};

}  // namespace atlas
