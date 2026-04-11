#pragma once

#include "db/idatabase.hpp"

#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <unordered_map>

namespace atlas
{

// ============================================================================
// XmlDatabase — file-based development/test database backend
//
// Storage layout:
//   <xml_dir>/meta.json          — { "next_dbid": N }
//   <xml_dir>/<TypeName>/        — one subdirectory per entity type
//   <xml_dir>/<TypeName>/<id>.bin — entity blob (binary)
//   <xml_dir>/<TypeName>/index.json — { "<identifier>": <dbid> }
//   <xml_dir>/auto_load.json     — [{ "type_id": N, "dbid": N }, ...]
//
// Callback execution mode:
//   - Default (deferred_mode_=false): callbacks are invoked synchronously
//     inside each API call, before it returns.
//   - Deferred (deferred_mode_=true): callbacks are queued and only invoked
//     when process_results() is called.  Use this mode in tests to simulate
//     the asynchronous behavior of the MySQL backend.
// ============================================================================

class XmlDatabase : public IDatabase
{
public:
    XmlDatabase() = default;
    ~XmlDatabase() override;

    /// Enable deferred-callback mode for testing async behavior.
    void set_deferred_mode(bool enabled) { deferred_mode_ = enabled; }

    [[nodiscard]] auto startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
        -> Result<void> override;
    void shutdown() override;

    void put_entity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                    std::span<const std::byte> blob, const std::string& identifier,
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

    void get_auto_load_entities(std::function<void(std::vector<EntityData>)> callback) override;

    void set_auto_load(DatabaseID dbid, uint16_t type_id, bool auto_load) override;

    void process_results() override;

private:
    // ---- Storage helpers ----
    [[nodiscard]] auto type_dir(uint16_t type_id) const -> std::filesystem::path;
    [[nodiscard]] auto type_name(uint16_t type_id) const -> std::string;
    [[nodiscard]] auto blob_path(uint16_t type_id, DatabaseID dbid) const -> std::filesystem::path;

    void load_meta();
    void save_meta();
    void load_index(uint16_t type_id);
    void save_index(uint16_t type_id);
    void load_auto_load();
    void save_auto_load();
    void load_checkouts();
    void save_checkouts();

    [[nodiscard]] auto read_blob(uint16_t type_id, DatabaseID dbid) const
        -> std::optional<std::vector<std::byte>>;
    void write_blob(uint16_t type_id, DatabaseID dbid, std::span<const std::byte> data);
    void delete_blob(uint16_t type_id, DatabaseID dbid);

    // ---- Deferred callback queue ----
    void fire_or_defer(std::function<void()> cb);

    // ---- State ----
    std::filesystem::path base_dir_;
    const EntityDefRegistry* entity_defs_{nullptr};
    DatabaseID next_dbid_{1};
    bool started_{false};
    bool deferred_mode_{false};

    // name → DBID index (per type_id)
    std::unordered_map<uint16_t, std::unordered_map<std::string, DatabaseID>> name_index_;

    // checkout: key = (type_id << 48) | (dbid & 0xFFFFFFFFFFFF)
    std::unordered_map<uint64_t, CheckoutInfo> checkouts_;

    // auto-load set: same key encoding
    std::set<uint64_t> auto_load_set_;

    // type_id → type name (from EntityDefRegistry)
    std::unordered_map<uint16_t, std::string> type_names_;

    // deferred callbacks
    std::deque<std::function<void()>> deferred_;

    static constexpr auto checkout_key(uint16_t type_id, DatabaseID dbid) -> uint64_t
    {
        return (static_cast<uint64_t>(type_id) << 48) |
               (static_cast<uint64_t>(dbid) & 0x0000FFFFFFFFFFFFULL);
    }
};

}  // namespace atlas
