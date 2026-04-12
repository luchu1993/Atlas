#include "db_xml/xml_database.hpp"

#include "foundation/log.hpp"
#include "serialization/data_section.hpp"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>

namespace atlas
{

// ============================================================================
// Destructor
// ============================================================================

XmlDatabase::~XmlDatabase()
{
    if (started_)
        shutdown();
}

void XmlDatabase::set_flush_policy(FlushPolicy policy)
{
    flush_policy_ = policy;
    if (started_ && flush_policy_ == FlushPolicy::Immediate)
        flush_dirty_state(true);
}

// ============================================================================
// startup / shutdown
// ============================================================================

auto XmlDatabase::startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void>
{
    base_dir_ = config.xml_dir;
    entity_defs_ = &entity_defs;

    // Build type_names_ map
    for (const auto& type : entity_defs.all_types())
        type_names_[type.type_id] = type.name;

    // Ensure base directory exists
    std::error_code ec;
    std::filesystem::create_directories(base_dir_, ec);
    if (ec)
    {
        return Error{ErrorCode::IoError, std::format("XmlDatabase: cannot create dir '{}': {}",
                                                     base_dir_.string(), ec.message())};
    }

    load_meta();
    load_auto_load();
    load_checkouts();

    started_ = true;
    ATLAS_LOG_INFO("XmlDatabase: started at '{}', next_dbid={}, flush_policy={}",
                   base_dir_.string(), next_dbid_,
                   flush_policy_ == FlushPolicy::Buffered ? "buffered" : "immediate");
    return {};
}

void XmlDatabase::shutdown()
{
    if (!started_)
        return;
    flush_dirty_state(true);
    started_ = false;
    ATLAS_LOG_INFO("XmlDatabase: shut down");
}

// ============================================================================
// CRUD
// ============================================================================

void XmlDatabase::put_entity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                             std::span<const std::byte> blob, const std::string& identifier,
                             std::function<void(PutResult)> callback)
{
    PutResult result;

    if (has_flag(flags, WriteFlags::Delete))
    {
        stage_blob_delete(type_id, dbid);
        // Remove name index entry
        auto it = name_index_.find(type_id);
        if (it != name_index_.end())
        {
            for (auto nit = it->second.begin(); nit != it->second.end(); ++nit)
            {
                if (nit->second == dbid)
                {
                    it->second.erase(nit);
                    mark_index_dirty(type_id);
                    break;
                }
            }
        }
        // Clear checkout if LogOff
        if (has_flag(flags, WriteFlags::LogOff))
        {
            checkouts_.erase(checkout_key(type_id, dbid));
            mark_checkouts_dirty();
        }
        result.success = true;
        result.dbid = dbid;
        flush_after_mutation();
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    if (has_flag(flags, WriteFlags::CreateNew) || dbid == kInvalidDBID)
    {
        dbid = next_dbid_++;
        mark_meta_dirty();
    }

    stage_blob_write(type_id, dbid, blob);

    // Update name index
    if (!identifier.empty())
    {
        load_index(type_id);
        name_index_[type_id][identifier] = dbid;
        mark_index_dirty(type_id);
    }

    // Update auto-load flags
    if (has_flag(flags, WriteFlags::AutoLoadOn))
    {
        auto_load_set_.insert(checkout_key(type_id, dbid));
        mark_auto_load_dirty();
    }
    else if (has_flag(flags, WriteFlags::AutoLoadOff))
    {
        auto_load_set_.erase(checkout_key(type_id, dbid));
        mark_auto_load_dirty();
    }

    // Clear checkout if LogOff
    if (has_flag(flags, WriteFlags::LogOff))
    {
        checkouts_.erase(checkout_key(type_id, dbid));
        mark_checkouts_dirty();
    }

    result.success = true;
    result.dbid = dbid;
    flush_after_mutation();
    fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
}

void XmlDatabase::get_entity(DatabaseID dbid, uint16_t type_id,
                             std::function<void(GetResult)> callback)
{
    GetResult result;
    auto blob = read_blob(type_id, dbid);
    if (!blob)
    {
        result.success = false;
        result.error = std::format("entity ({},{}) not found", type_id, dbid);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    result.success = true;
    result.data.dbid = dbid;
    result.data.type_id = type_id;
    result.data.blob = std::move(*blob);

    auto cit = checkouts_.find(checkout_key(type_id, dbid));
    if (cit != checkouts_.end())
        result.checked_out_by = cit->second;

    fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                  { cb(std::move(result)); });
}

void XmlDatabase::del_entity(DatabaseID dbid, uint16_t type_id,
                             std::function<void(DelResult)> callback)
{
    DelResult result;
    if (!read_blob(type_id, dbid).has_value())
    {
        result.success = false;
        result.error = std::format("entity ({},{}) not found", type_id, dbid);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }
    stage_blob_delete(type_id, dbid);

    // Remove name index entry
    auto it = name_index_.find(type_id);
    if (it != name_index_.end())
    {
        for (auto nit = it->second.begin(); nit != it->second.end(); ++nit)
        {
            if (nit->second == dbid)
            {
                it->second.erase(nit);
                mark_index_dirty(type_id);
                break;
            }
        }
    }

    checkouts_.erase(checkout_key(type_id, dbid));
    auto_load_set_.erase(checkout_key(type_id, dbid));
    mark_checkouts_dirty();
    mark_auto_load_dirty();

    result.success = true;
    flush_after_mutation();
    fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
}

void XmlDatabase::lookup_by_name(uint16_t type_id, const std::string& identifier,
                                 std::function<void(LookupResult)> callback)
{
    load_index(type_id);
    LookupResult result;
    auto it = name_index_.find(type_id);
    if (it != name_index_.end())
    {
        auto nit = it->second.find(identifier);
        if (nit != it->second.end())
        {
            result.found = true;
            result.dbid = nit->second;
        }
    }
    fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
}

// ============================================================================
// Checkout
// ============================================================================

void XmlDatabase::checkout_entity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                                  std::function<void(GetResult)> callback)
{
    GetResult result;
    auto blob = read_blob(type_id, dbid);
    if (!blob)
    {
        result.success = false;
        result.error = std::format("checkout: entity ({},{}) not found", type_id, dbid);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }

    auto key = checkout_key(type_id, dbid);
    auto cit = checkouts_.find(key);
    if (cit != checkouts_.end())
    {
        // Already checked out
        result.success = true;
        result.data.dbid = dbid;
        result.data.type_id = type_id;
        result.data.blob = std::move(*blob);
        result.checked_out_by = cit->second;
        fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                      { cb(std::move(result)); });
        return;
    }

    checkouts_[key] = new_owner;
    mark_checkouts_dirty();

    result.success = true;
    result.data.dbid = dbid;
    result.data.type_id = type_id;
    result.data.blob = std::move(*blob);
    flush_after_mutation();
    fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                  { cb(std::move(result)); });
}

void XmlDatabase::checkout_entity_by_name(uint16_t type_id, const std::string& identifier,
                                          const CheckoutInfo& new_owner,
                                          std::function<void(GetResult)> callback)
{
    load_index(type_id);
    auto it = name_index_.find(type_id);
    if (it == name_index_.end() || !it->second.count(identifier))
    {
        GetResult result;
        result.success = false;
        result.error = std::format("checkout_by_name: '{}' not found", identifier);
        fire_or_defer([cb = std::move(callback), result]() mutable { cb(result); });
        return;
    }
    DatabaseID dbid = it->second.at(identifier);
    checkout_entity(dbid, type_id, new_owner, std::move(callback));
}

void XmlDatabase::clear_checkout(DatabaseID dbid, uint16_t type_id,
                                 std::function<void(bool)> callback)
{
    bool erased = checkouts_.erase(checkout_key(type_id, dbid)) > 0;
    if (erased)
    {
        mark_checkouts_dirty();
        flush_after_mutation();
    }
    fire_or_defer([cb = std::move(callback), erased]() mutable { cb(erased); });
}

void XmlDatabase::clear_checkouts_for_address(const Address& base_addr,
                                              std::function<void(int cleared_count)> callback)
{
    int count = 0;
    for (auto it = checkouts_.begin(); it != checkouts_.end();)
    {
        if (it->second.base_addr == base_addr)
        {
            it = checkouts_.erase(it);
            ++count;
        }
        else
        {
            ++it;
        }
    }
    if (count > 0)
    {
        mark_checkouts_dirty();
        flush_after_mutation();
    }
    fire_or_defer([cb = std::move(callback), count]() mutable { cb(count); });
}

// ============================================================================
// Auto-load
// ============================================================================

void XmlDatabase::get_auto_load_entities(std::function<void(std::vector<EntityData>)> callback)
{
    std::vector<EntityData> result;
    for (auto key : auto_load_set_)
    {
        auto type_id = static_cast<uint16_t>(key >> 48);
        auto dbid = static_cast<DatabaseID>(key & 0x0000FFFFFFFFFFFFULL);
        auto blob = read_blob(type_id, dbid);
        if (blob)
        {
            EntityData ed;
            ed.dbid = dbid;
            ed.type_id = type_id;
            ed.blob = std::move(*blob);
            result.push_back(std::move(ed));
        }
    }
    fire_or_defer([cb = std::move(callback), result = std::move(result)]() mutable
                  { cb(std::move(result)); });
}

void XmlDatabase::set_auto_load(DatabaseID dbid, uint16_t type_id, bool auto_load)
{
    auto key = checkout_key(type_id, dbid);
    if (auto_load)
        auto_load_set_.insert(key);
    else
        auto_load_set_.erase(key);
    mark_auto_load_dirty();
    flush_after_mutation();
}

// ============================================================================
// process_results
// ============================================================================

void XmlDatabase::process_results()
{
    flush_dirty_state();

    if (!deferred_mode_)
        return;

    int budget = kMaxCallbacksPerTick;
    while (!deferred_.empty() && budget-- > 0)
    {
        auto cb = std::move(deferred_.front());
        deferred_.pop_front();
        cb();
    }
}

void XmlDatabase::mark_checkout_cleared(DatabaseID dbid, uint16_t type_id)
{
    if (checkouts_.erase(checkout_key(type_id, dbid)) > 0)
    {
        mark_checkouts_dirty();
        flush_after_mutation();
    }
}

void XmlDatabase::mark_meta_dirty()
{
    meta_dirty_ = true;
    if (next_metadata_flush_deadline_ == TimePoint{})
        next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::mark_index_dirty(uint16_t type_id)
{
    dirty_indexes_.insert(type_id);
    if (next_metadata_flush_deadline_ == TimePoint{})
        next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::mark_auto_load_dirty()
{
    auto_load_dirty_ = true;
    if (next_metadata_flush_deadline_ == TimePoint{})
        next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::mark_checkouts_dirty()
{
    checkouts_dirty_ = true;
    if (next_metadata_flush_deadline_ == TimePoint{})
        next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::flush_after_mutation()
{
    if (flush_policy_ == FlushPolicy::Immediate)
    {
        flush_dirty_state(true);
        return;
    }

    if (pending_blob_writes_.size() >= kMaxPendingBlobWrites ||
        pending_blob_bytes_ >= kMaxPendingBlobBytes)
    {
        flush_dirty_state(true);
    }
}

void XmlDatabase::flush_dirty_state(bool force)
{
    if (!started_)
    {
        return;
    }

    const bool has_blobs = !pending_blob_writes_.empty();
    const bool has_metadata =
        meta_dirty_ || auto_load_dirty_ || checkouts_dirty_ || !dirty_indexes_.empty();

    if (!has_blobs && !has_metadata)
    {
        next_flush_deadline_ = {};
        next_metadata_flush_deadline_ = {};
        return;
    }

    const auto now = Clock::now();

    if (has_blobs)
    {
        const bool blob_ready =
            force || (next_flush_deadline_ != TimePoint{} && now >= next_flush_deadline_);
        if (blob_ready)
        {
            int blob_budget =
                force ? static_cast<int>(pending_blob_writes_.size()) : kMaxBlobWritesPerFlush;
            flush_pending_blob_writes(blob_budget);
        }

        if (pending_blob_writes_.empty())
            next_flush_deadline_ = {};
        else if (next_flush_deadline_ == TimePoint{})
            next_flush_deadline_ = now + kFlushInterval;
    }

    if (has_metadata)
    {
        const bool metadata_ready = force || (next_metadata_flush_deadline_ != TimePoint{} &&
                                              now >= next_metadata_flush_deadline_);
        if (metadata_ready)
        {
            if (meta_dirty_)
            {
                save_meta();
                meta_dirty_ = false;
            }
            if (!dirty_indexes_.empty())
            {
                for (uint16_t type_id : dirty_indexes_)
                    save_index(type_id);
                dirty_indexes_.clear();
            }
            if (auto_load_dirty_)
            {
                save_auto_load();
                auto_load_dirty_ = false;
            }
            if (checkouts_dirty_)
            {
                save_checkouts();
                checkouts_dirty_ = false;
            }
            next_metadata_flush_deadline_ = {};
        }
    }
}

// ============================================================================
// Private helpers — paths
// ============================================================================

auto XmlDatabase::type_name(uint16_t type_id) const -> std::string
{
    auto it = type_names_.find(type_id);
    if (it != type_names_.end())
        return it->second;
    return std::format("type_{}", type_id);
}

auto XmlDatabase::type_dir(uint16_t type_id) const -> std::filesystem::path
{
    return base_dir_ / type_name(type_id);
}

auto XmlDatabase::blob_path(uint16_t type_id, DatabaseID dbid) const -> std::filesystem::path
{
    return type_dir(type_id) / std::format("{}.bin", dbid);
}

// ============================================================================
// Private helpers — I/O
// ============================================================================

void XmlDatabase::load_meta()
{
    auto path = base_dir_ / "meta.json";
    if (!std::filesystem::exists(path))
        return;
    auto tree = DataSection::from_json(path);
    if (!tree)
        return;
    next_dbid_ = static_cast<DatabaseID>((*tree)->root()->read_uint("next_dbid", 1));
}

void XmlDatabase::save_meta()
{
    auto path = base_dir_ / "meta.json";
    std::ofstream f(path);
    if (f)
        f << std::format("{{\"next_dbid\":{}}}\n", next_dbid_);
}

void XmlDatabase::load_index(uint16_t type_id)
{
    if (name_index_.count(type_id))
        return;  // already loaded

    auto path = type_dir(type_id) / "index.json";
    if (!std::filesystem::exists(path))
    {
        name_index_[type_id] = {};
        return;
    }
    auto tree = DataSection::from_json(path);
    if (!tree)
    {
        name_index_[type_id] = {};
        return;
    }

    auto* root = (*tree)->root();
    auto& idx = name_index_[type_id];
    for (auto* child : root->children())
    {
        try
        {
            idx[std::string(child->name())] =
                static_cast<DatabaseID>(std::stoll(std::string(child->value())));
        }
        catch (...)
        {
        }
    }
}

void XmlDatabase::save_index(uint16_t type_id)
{
    auto it = name_index_.find(type_id);
    if (it == name_index_.end())
        return;

    std::error_code ec;
    std::filesystem::create_directories(type_dir(type_id), ec);

    auto path = type_dir(type_id) / "index.json";
    std::ofstream f(path);
    if (!f)
        return;

    f << "{\n";
    bool first = true;
    for (const auto& [name, dbid] : it->second)
    {
        if (!first)
            f << ",\n";
        // Escape name minimally
        std::string escaped = name;
        // Replace " → \"
        for (size_t i = 0; i < escaped.size(); ++i)
            if (escaped[i] == '"')
                escaped.replace(i, 1, "\\\""), i++;
        f << std::format("  \"{}\": {}", escaped, dbid);
        first = false;
    }
    f << "\n}\n";
}

void XmlDatabase::load_auto_load()
{
    auto path = base_dir_ / "auto_load.json";
    if (!std::filesystem::exists(path))
        return;
    auto tree = DataSection::from_json(path);
    if (!tree)
        return;

    auto* root = (*tree)->root();
    for (auto* entry : root->children())
    {
        auto type_id = static_cast<uint16_t>(entry->read_uint("type_id", 0));
        auto dbid = static_cast<DatabaseID>(entry->read_uint("dbid", 0));
        if (type_id != 0 && dbid != 0)
            auto_load_set_.insert(checkout_key(type_id, dbid));
    }
}

void XmlDatabase::save_auto_load()
{
    auto path = base_dir_ / "auto_load.json";
    std::ofstream f(path);
    if (!f)
        return;

    f << "[\n";
    bool first = true;
    for (auto key : auto_load_set_)
    {
        auto type_id = static_cast<uint16_t>(key >> 48);
        auto dbid = static_cast<DatabaseID>(key & 0x0000FFFFFFFFFFFFULL);
        if (!first)
            f << ",\n";
        f << std::format("  {{\"type_id\":{},\"dbid\":{}}}", type_id, dbid);
        first = false;
    }
    f << "\n]\n";
}

void XmlDatabase::load_checkouts()
{
    auto path = base_dir_ / "checkouts.json";
    if (!std::filesystem::exists(path))
        return;
    auto tree = DataSection::from_json(path);
    if (!tree)
        return;

    auto* root = (*tree)->root();
    for (auto* entry : root->children())
    {
        auto type_id = static_cast<uint16_t>(entry->read_uint("type_id", 0));
        auto dbid = static_cast<DatabaseID>(entry->read_uint("dbid", 0));
        if (type_id == 0 || dbid == 0)
            continue;
        CheckoutInfo info;
        auto ip = entry->read_uint("base_ip", 0);
        auto port = static_cast<uint16_t>(entry->read_uint("base_port", 0));
        info.base_addr = Address(ip, port);
        info.app_id = entry->read_uint("app_id", 0);
        info.entity_id = entry->read_uint("entity_id", 0);
        checkouts_[checkout_key(type_id, dbid)] = info;
    }
}

void XmlDatabase::save_checkouts()
{
    auto path = base_dir_ / "checkouts.json";
    std::ofstream f(path);
    if (!f)
        return;

    f << "[\n";
    bool first = true;
    for (const auto& [key, info] : checkouts_)
    {
        auto type_id = static_cast<uint16_t>(key >> 48);
        auto dbid = static_cast<DatabaseID>(key & 0x0000FFFFFFFFFFFFULL);
        if (!first)
            f << ",\n";
        f << std::format(
            "  {{\"type_id\":{},\"dbid\":{},\"base_ip\":{},\"base_port\":{}"
            ",\"app_id\":{},\"entity_id\":{}}}",
            type_id, dbid, info.base_addr.ip(), info.base_addr.port(), info.app_id, info.entity_id);
        first = false;
    }
    f << "\n]\n";
}

auto XmlDatabase::read_blob(uint16_t type_id, DatabaseID dbid) const
    -> std::optional<std::vector<std::byte>>
{
    auto key = checkout_key(type_id, dbid);

    if (auto pending = pending_blob_writes_.find(key); pending != pending_blob_writes_.end())
    {
        if (pending->second.deleted)
            return std::nullopt;
        return pending->second.data;
    }

    if (auto cached = blob_cache_.find(key); cached != blob_cache_.end())
        return cached->second;

    auto path = blob_path(type_id, dbid);
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;

    f.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    std::vector<std::byte> data(size);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

    blob_cache_[key] = data;
    return data;
}

void XmlDatabase::write_blob(uint16_t type_id, DatabaseID dbid, std::span<const std::byte> data)
{
    std::error_code ec;
    std::filesystem::create_directories(type_dir(type_id), ec);
    std::ofstream f(blob_path(type_id, dbid), std::ios::binary | std::ios::trunc);
    if (f)
        f.write(reinterpret_cast<const char*>(data.data()),
                static_cast<std::streamsize>(data.size()));
}

void XmlDatabase::delete_blob(uint16_t type_id, DatabaseID dbid)
{
    std::error_code ec;
    std::filesystem::remove(blob_path(type_id, dbid), ec);
}

void XmlDatabase::stage_blob_write(uint16_t type_id, DatabaseID dbid,
                                   std::span<const std::byte> data)
{
    auto key = checkout_key(type_id, dbid);
    auto& pending = pending_blob_writes_[key];
    if (!pending.deleted)
        pending_blob_bytes_ -= pending.data.size();
    pending.type_id = type_id;
    pending.dbid = dbid;
    pending.data.assign(data.begin(), data.end());
    pending.deleted = false;
    pending_blob_bytes_ += pending.data.size();
    if (next_flush_deadline_ == TimePoint{})
    {
        next_flush_deadline_ = Clock::now() + kFlushInterval;
    }
}

void XmlDatabase::stage_blob_delete(uint16_t type_id, DatabaseID dbid)
{
    auto key = checkout_key(type_id, dbid);
    auto& pending = pending_blob_writes_[key];
    if (!pending.deleted)
        pending_blob_bytes_ -= pending.data.size();
    pending.type_id = type_id;
    pending.dbid = dbid;
    pending.data.clear();
    pending.deleted = true;
    blob_cache_.erase(key);
    if (next_flush_deadline_ == TimePoint{})
    {
        next_flush_deadline_ = Clock::now() + kFlushInterval;
    }
}

void XmlDatabase::flush_pending_blob_writes(int budget)
{
    auto it = pending_blob_writes_.begin();
    int written = 0;
    while (it != pending_blob_writes_.end() && written < budget)
    {
        if (!it->second.deleted)
            pending_blob_bytes_ -= it->second.data.size();
        if (it->second.deleted)
        {
            delete_blob(it->second.type_id, it->second.dbid);
            blob_cache_.erase(it->first);
        }
        else
        {
            write_blob(it->second.type_id, it->second.dbid, it->second.data);
            blob_cache_[it->first] = std::move(it->second.data);
        }
        it = pending_blob_writes_.erase(it);
        ++written;
    }
}

// ============================================================================
// Private helpers — deferred dispatch
// ============================================================================

void XmlDatabase::fire_or_defer(std::function<void()> cb)
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
