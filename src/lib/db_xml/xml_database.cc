#include "db_xml/xml_database.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <string_view>

#include "foundation/log.h"
#include "serialization/data_section.h"

namespace {

auto EscapeJsonString(std::string_view value) -> std::string {
  std::string escaped;
  escaped.reserve(value.size());
  for (char ch : value) {
    switch (ch) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        escaped += ch;
        break;
    }
  }
  return escaped;
}

}  // namespace

namespace atlas {

// ============================================================================
// Destructor
// ============================================================================

XmlDatabase::~XmlDatabase() {
  if (started_) Shutdown();
}

void XmlDatabase::SetFlushPolicy(FlushPolicy policy) {
  flush_policy_ = policy;
  if (started_ && flush_policy_ == FlushPolicy::kImmediate) FlushDirtyState(true);
}

// ============================================================================
// startup / shutdown
// ============================================================================

auto XmlDatabase::Startup(const DatabaseConfig& config, const EntityDefRegistry& entity_defs)
    -> Result<void> {
  base_dir_ = config.xml_dir;
  entity_defs_ = &entity_defs;

  // Build type_names_ map
  for (const auto& type : entity_defs.AllTypes()) type_names_[type.type_id] = type.name;

  // Ensure base directory exists
  std::error_code ec;
  std::filesystem::create_directories(base_dir_, ec);
  if (ec) {
    return Error{ErrorCode::kIoError, std::format("XmlDatabase: cannot create dir '{}': {}",
                                                  base_dir_.string(), ec.message())};
  }

  LoadMeta();
  LoadAutoLoad();
  LoadCheckouts();
  LoadPasswordHashes();

  started_ = true;
  ATLAS_LOG_INFO("XmlDatabase: started at '{}', next_dbid={}, flush_policy={}", base_dir_.string(),
                 next_dbid_, flush_policy_ == FlushPolicy::kBuffered ? "buffered" : "immediate");
  return {};
}

void XmlDatabase::Shutdown() {
  if (!started_) return;
  FlushDirtyState(true);
  started_ = false;
  ATLAS_LOG_INFO("XmlDatabase: shut down");
}

// ============================================================================
// CRUD
// ============================================================================

void XmlDatabase::PutEntity(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                            std::span<const std::byte> blob, const std::string& identifier,
                            std::function<void(PutResult)> callback) {
  PutResult result;
  const auto kEy = CheckoutKey(type_id, dbid);

  if (HasFlag(flags, WriteFlags::kDelete)) {
    StageBlobDelete(type_id, dbid);
    // Remove name index entry
    auto it = name_index_.find(type_id);
    if (it != name_index_.end()) {
      for (auto nit = it->second.begin(); nit != it->second.end(); ++nit) {
        if (nit->second == dbid) {
          it->second.erase(nit);
          MarkIndexDirty(type_id);
          break;
        }
      }
    }
    // Clear checkout if LogOff
    if (HasFlag(flags, WriteFlags::kLogOff)) {
      checkouts_.erase(kEy);
      MarkCheckoutsDirty();
    }
    if (password_hashes_.erase(kEy) > 0) MarkPasswordHashesDirty();
    result.success = true;
    result.dbid = dbid;
    FlushAfterMutation();
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  if (HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID) {
    dbid = next_dbid_++;
    MarkMetaDirty();
  }

  StageBlobWrite(type_id, dbid, blob);

  // Update name index
  if (!identifier.empty()) {
    LoadIndex(type_id);
    name_index_[type_id][identifier] = dbid;
    MarkIndexDirty(type_id);
  }

  // Update auto-load flags
  if (HasFlag(flags, WriteFlags::kAutoLoadOn)) {
    auto_load_set_.insert(CheckoutKey(type_id, dbid));
    MarkAutoLoadDirty();
  } else if (HasFlag(flags, WriteFlags::kAutoLoadOff)) {
    auto_load_set_.erase(CheckoutKey(type_id, dbid));
    MarkAutoLoadDirty();
  }

  // Clear checkout if LogOff
  if (HasFlag(flags, WriteFlags::kLogOff)) {
    checkouts_.erase(CheckoutKey(type_id, dbid));
    MarkCheckoutsDirty();
  }

  result.success = true;
  result.dbid = dbid;
  FlushAfterMutation();
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void XmlDatabase::PutEntityWithPassword(DatabaseID dbid, uint16_t type_id, WriteFlags flags,
                                        std::span<const std::byte> blob,
                                        const std::string& identifier,
                                        const std::string& password_hash,
                                        std::function<void(PutResult)> callback) {
  PutResult result;

  if (HasFlag(flags, WriteFlags::kDelete)) {
    PutEntity(dbid, type_id, flags, blob, identifier, std::move(callback));
    return;
  }

  if (HasFlag(flags, WriteFlags::kCreateNew) || dbid == kInvalidDBID) {
    dbid = next_dbid_++;
    MarkMetaDirty();
  }

  StageBlobWrite(type_id, dbid, blob);

  if (!identifier.empty()) {
    LoadIndex(type_id);
    name_index_[type_id][identifier] = dbid;
    MarkIndexDirty(type_id);
  }

  if (HasFlag(flags, WriteFlags::kAutoLoadOn)) {
    auto_load_set_.insert(CheckoutKey(type_id, dbid));
    MarkAutoLoadDirty();
  } else if (HasFlag(flags, WriteFlags::kAutoLoadOff)) {
    auto_load_set_.erase(CheckoutKey(type_id, dbid));
    MarkAutoLoadDirty();
  }

  if (HasFlag(flags, WriteFlags::kLogOff)) {
    checkouts_.erase(CheckoutKey(type_id, dbid));
    MarkCheckoutsDirty();
  }

  const auto kEy = CheckoutKey(type_id, dbid);
  if (password_hash.empty()) {
    if (password_hashes_.erase(kEy) > 0) MarkPasswordHashesDirty();
  } else {
    auto it = password_hashes_.find(kEy);
    if (it == password_hashes_.end() || it->second != password_hash) {
      password_hashes_[kEy] = password_hash;
      MarkPasswordHashesDirty();
    }
  }

  result.success = true;
  result.dbid = dbid;
  FlushAfterMutation();
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void XmlDatabase::GetEntity(DatabaseID dbid, uint16_t type_id,
                            std::function<void(GetResult)> callback) {
  GetResult result;
  auto blob = ReadBlob(type_id, dbid);
  if (!blob) {
    result.success = false;
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  result.success = true;
  result.data.dbid = dbid;
  result.data.type_id = type_id;
  result.data.blob = std::move(*blob);

  auto cit = checkouts_.find(CheckoutKey(type_id, dbid));
  if (cit != checkouts_.end()) result.checked_out_by = cit->second;

  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void XmlDatabase::DelEntity(DatabaseID dbid, uint16_t type_id,
                            std::function<void(DelResult)> callback) {
  DelResult result;
  if (!ReadBlob(type_id, dbid).has_value()) {
    result.success = false;
    result.error = std::format("entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  StageBlobDelete(type_id, dbid);

  // Remove name index entry
  auto it = name_index_.find(type_id);
  if (it != name_index_.end()) {
    for (auto nit = it->second.begin(); nit != it->second.end(); ++nit) {
      if (nit->second == dbid) {
        it->second.erase(nit);
        MarkIndexDirty(type_id);
        break;
      }
    }
  }

  checkouts_.erase(CheckoutKey(type_id, dbid));
  auto_load_set_.erase(CheckoutKey(type_id, dbid));
  if (password_hashes_.erase(CheckoutKey(type_id, dbid)) > 0) MarkPasswordHashesDirty();
  MarkCheckoutsDirty();
  MarkAutoLoadDirty();

  result.success = true;
  FlushAfterMutation();
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

void XmlDatabase::LookupByName(uint16_t type_id, const std::string& identifier,
                               std::function<void(LookupResult)> callback) {
  LoadIndex(type_id);
  LookupResult result;
  auto it = name_index_.find(type_id);
  if (it != name_index_.end()) {
    auto nit = it->second.find(identifier);
    if (nit != it->second.end()) {
      result.found = true;
      result.dbid = nit->second;
      if (auto pw = password_hashes_.find(CheckoutKey(type_id, nit->second));
          pw != password_hashes_.end()) {
        result.password_hash = pw->second;
      }
    }
  }
  FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
}

// ============================================================================
// Checkout
// ============================================================================

void XmlDatabase::CheckoutEntity(DatabaseID dbid, uint16_t type_id, const CheckoutInfo& new_owner,
                                 std::function<void(GetResult)> callback) {
  GetResult result;
  auto blob = ReadBlob(type_id, dbid);
  if (!blob) {
    result.success = false;
    result.error = std::format("checkout: entity ({},{}) not found", type_id, dbid);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }

  auto key = CheckoutKey(type_id, dbid);
  auto cit = checkouts_.find(key);
  if (cit != checkouts_.end()) {
    // Already checked out
    result.success = true;
    result.data.dbid = dbid;
    result.data.type_id = type_id;
    result.data.blob = std::move(*blob);
    result.checked_out_by = cit->second;
    FireOrDefer([cb = std::move(callback), result = std::move(result)]() mutable {
      cb(std::move(result));
    });
    return;
  }

  checkouts_[key] = new_owner;
  MarkCheckoutsDirty();

  result.success = true;
  result.data.dbid = dbid;
  result.data.type_id = type_id;
  result.data.blob = std::move(*blob);
  FlushAfterMutation();
  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void XmlDatabase::CheckoutEntityByName(uint16_t type_id, const std::string& identifier,
                                       const CheckoutInfo& new_owner,
                                       std::function<void(GetResult)> callback) {
  LoadIndex(type_id);
  auto it = name_index_.find(type_id);
  if (it == name_index_.end() || !it->second.count(identifier)) {
    GetResult result;
    result.success = false;
    result.error = std::format("checkout_by_name: '{}' not found", identifier);
    FireOrDefer([cb = std::move(callback), result]() mutable { cb(result); });
    return;
  }
  DatabaseID dbid = it->second.at(identifier);
  CheckoutEntity(dbid, type_id, new_owner, std::move(callback));
}

void XmlDatabase::ClearCheckout(DatabaseID dbid, uint16_t type_id,
                                std::function<void(bool)> callback) {
  bool erased = checkouts_.erase(CheckoutKey(type_id, dbid)) > 0;
  if (erased) {
    MarkCheckoutsDirty();
    FlushAfterMutation();
  }
  FireOrDefer([cb = std::move(callback), erased]() mutable { cb(erased); });
}

void XmlDatabase::ClearCheckoutsForAddress(const Address& base_addr,
                                           std::function<void(int cleared_count)> callback) {
  int count = 0;
  for (auto it = checkouts_.begin(); it != checkouts_.end();) {
    if (it->second.base_addr == base_addr) {
      it = checkouts_.erase(it);
      ++count;
    } else {
      ++it;
    }
  }
  if (count > 0) {
    MarkCheckoutsDirty();
    FlushAfterMutation();
  }
  FireOrDefer([cb = std::move(callback), count]() mutable { cb(count); });
}

// ============================================================================
// Auto-load
// ============================================================================

void XmlDatabase::GetAutoLoadEntities(std::function<void(std::vector<EntityData>)> callback) {
  std::vector<EntityData> result;
  for (auto key : auto_load_set_) {
    auto type_id = static_cast<uint16_t>(key >> 48);
    auto dbid = static_cast<DatabaseID>(key & 0x0000FFFFFFFFFFFFULL);
    auto blob = ReadBlob(type_id, dbid);
    if (blob) {
      EntityData ed;
      ed.dbid = dbid;
      ed.type_id = type_id;
      ed.blob = std::move(*blob);
      result.push_back(std::move(ed));
    }
  }
  FireOrDefer(
      [cb = std::move(callback), result = std::move(result)]() mutable { cb(std::move(result)); });
}

void XmlDatabase::SetAutoLoad(DatabaseID dbid, uint16_t type_id, bool auto_load) {
  auto key = CheckoutKey(type_id, dbid);
  if (auto_load)
    auto_load_set_.insert(key);
  else
    auto_load_set_.erase(key);
  MarkAutoLoadDirty();
  FlushAfterMutation();
}

// ============================================================================
// process_results
// ============================================================================

void XmlDatabase::ProcessResults() {
  FlushDirtyState();

  if (!deferred_mode_) return;

  int budget = kMaxCallbacksPerTick;
  while (!deferred_.empty() && budget-- > 0) {
    auto cb = std::move(deferred_.front());
    deferred_.pop_front();
    cb();
  }
}

void XmlDatabase::LoadEntityIdCounter(std::function<void(EntityID next_id)> callback) {
  EntityID next_id = 1;
  auto path = base_dir_ / "entity_id_counter.json";
  if (std::filesystem::exists(path)) {
    auto tree = DataSection::FromJson(path);
    if (tree) {
      next_id = static_cast<EntityID>((*tree)->Root()->ReadUint("next_id", 1));
    }
  }
  FireOrDefer([cb = std::move(callback), next_id]() { cb(next_id); });
}

void XmlDatabase::SaveEntityIdCounter(EntityID next_id,
                                      std::function<void(bool success)> callback) {
  auto path = base_dir_ / "entity_id_counter.json";
  std::ofstream f(path);
  bool ok = false;
  if (f) {
    f << std::format("{{\"next_id\":{}}}\n", next_id);
    ok = f.good();
  }
  FireOrDefer([cb = std::move(callback), ok]() { cb(ok); });
}

void XmlDatabase::MarkCheckoutCleared(DatabaseID dbid, uint16_t type_id) {
  if (checkouts_.erase(CheckoutKey(type_id, dbid)) > 0) {
    MarkCheckoutsDirty();
    FlushAfterMutation();
  }
}

void XmlDatabase::MarkMetaDirty() {
  meta_dirty_ = true;
  if (next_metadata_flush_deadline_ == TimePoint{})
    next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::MarkIndexDirty(uint16_t type_id) {
  dirty_indexes_.insert(type_id);
  if (next_metadata_flush_deadline_ == TimePoint{})
    next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::MarkAutoLoadDirty() {
  auto_load_dirty_ = true;
  if (next_metadata_flush_deadline_ == TimePoint{})
    next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::MarkCheckoutsDirty() {
  checkouts_dirty_ = true;
  if (next_metadata_flush_deadline_ == TimePoint{})
    next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::MarkPasswordHashesDirty() {
  password_hashes_dirty_ = true;
  if (next_metadata_flush_deadline_ == TimePoint{})
    next_metadata_flush_deadline_ = Clock::now() + kMetadataFlushInterval;
}

void XmlDatabase::FlushAfterMutation() {
  if (flush_policy_ == FlushPolicy::kImmediate) {
    FlushDirtyState(true);
    return;
  }

  if (pending_blob_writes_.size() >= kMaxPendingBlobWrites ||
      pending_blob_bytes_ >= kMaxPendingBlobBytes) {
    FlushDirtyState(true);
  }
}

void XmlDatabase::FlushDirtyState(bool force) {
  if (!started_) {
    return;
  }

  const bool kHasBlobs = !pending_blob_writes_.empty();
  const bool kHasMetadata = meta_dirty_ || auto_load_dirty_ || checkouts_dirty_ ||
                            password_hashes_dirty_ || !dirty_indexes_.empty();

  if (!kHasBlobs && !kHasMetadata) {
    next_flush_deadline_ = {};
    next_metadata_flush_deadline_ = {};
    return;
  }

  const auto kNow = Clock::now();

  if (kHasBlobs) {
    const bool kBlobReady =
        force || (next_flush_deadline_ != TimePoint{} && kNow >= next_flush_deadline_);
    if (kBlobReady) {
      int blob_budget =
          force ? static_cast<int>(pending_blob_writes_.size()) : kMaxBlobWritesPerFlush;
      FlushPendingBlobWrites(blob_budget);
    }

    if (pending_blob_writes_.empty())
      next_flush_deadline_ = {};
    else if (next_flush_deadline_ == TimePoint{})
      next_flush_deadline_ = kNow + kFlushInterval;
  }

  if (kHasMetadata) {
    const bool kMetadataReady = force || (next_metadata_flush_deadline_ != TimePoint{} &&
                                          kNow >= next_metadata_flush_deadline_);
    if (kMetadataReady) {
      if (meta_dirty_) {
        SaveMeta();
        meta_dirty_ = false;
      }
      if (!dirty_indexes_.empty()) {
        for (uint16_t type_id : dirty_indexes_) SaveIndex(type_id);
        dirty_indexes_.clear();
      }
      if (auto_load_dirty_) {
        SaveAutoLoad();
        auto_load_dirty_ = false;
      }
      if (checkouts_dirty_) {
        SaveCheckouts();
        checkouts_dirty_ = false;
      }
      if (password_hashes_dirty_) {
        SavePasswordHashes();
        password_hashes_dirty_ = false;
      }
      next_metadata_flush_deadline_ = {};
    }
  }
}

// ============================================================================
// Private helpers — paths
// ============================================================================

auto XmlDatabase::TypeName(uint16_t type_id) const -> std::string {
  auto it = type_names_.find(type_id);
  if (it != type_names_.end()) return it->second;
  return std::format("type_{}", type_id);
}

auto XmlDatabase::TypeDir(uint16_t type_id) const -> std::filesystem::path {
  return base_dir_ / TypeName(type_id);
}

auto XmlDatabase::BlobPath(uint16_t type_id, DatabaseID dbid) const -> std::filesystem::path {
  return TypeDir(type_id) / std::format("{}.bin", dbid);
}

// ============================================================================
// Private helpers — I/O
// ============================================================================

void XmlDatabase::LoadMeta() {
  auto path = base_dir_ / "meta.json";
  if (!std::filesystem::exists(path)) return;
  auto tree = DataSection::FromJson(path);
  if (!tree) return;
  next_dbid_ = static_cast<DatabaseID>((*tree)->Root()->ReadUint("next_dbid", 1));
}

void XmlDatabase::SaveMeta() {
  auto path = base_dir_ / "meta.json";
  std::ofstream f(path);
  if (f) f << std::format("{{\"next_dbid\":{}}}\n", next_dbid_);
}

void XmlDatabase::LoadIndex(uint16_t type_id) {
  if (name_index_.count(type_id)) return;  // already loaded

  auto path = TypeDir(type_id) / "index.json";
  if (!std::filesystem::exists(path)) {
    name_index_[type_id] = {};
    return;
  }
  auto tree = DataSection::FromJson(path);
  if (!tree) {
    name_index_[type_id] = {};
    return;
  }

  auto* root = (*tree)->Root();
  auto& idx = name_index_[type_id];
  for (auto* child : root->Children()) {
    try {
      idx[std::string(child->Name())] =
          static_cast<DatabaseID>(std::stoll(std::string(child->Value())));
    } catch (...) {}
  }
}

void XmlDatabase::SaveIndex(uint16_t type_id) {
  auto it = name_index_.find(type_id);
  if (it == name_index_.end()) return;

  std::error_code ec;
  std::filesystem::create_directories(TypeDir(type_id), ec);

  auto path = TypeDir(type_id) / "index.json";
  std::ofstream f(path);
  if (!f) return;

  f << "{\n";
  bool first = true;
  for (const auto& [name, dbid] : it->second) {
    if (!first) f << ",\n";
    f << std::format("  \"{}\": {}", EscapeJsonString(name), dbid);
    first = false;
  }
  f << "\n}\n";
}

void XmlDatabase::LoadAutoLoad() {
  auto path = base_dir_ / "auto_load.json";
  if (!std::filesystem::exists(path)) return;
  auto tree = DataSection::FromJson(path);
  if (!tree) return;

  auto* root = (*tree)->Root();
  for (auto* entry : root->Children()) {
    auto type_id = static_cast<uint16_t>(entry->ReadUint("type_id", 0));
    auto dbid = static_cast<DatabaseID>(entry->ReadUint("dbid", 0));
    if (type_id != 0 && dbid != 0) auto_load_set_.insert(CheckoutKey(type_id, dbid));
  }
}

void XmlDatabase::SaveAutoLoad() {
  auto path = base_dir_ / "auto_load.json";
  std::ofstream f(path);
  if (!f) return;

  f << "[\n";
  bool first = true;
  for (auto key : auto_load_set_) {
    auto type_id = static_cast<uint16_t>(key >> 48);
    auto dbid = static_cast<DatabaseID>(key & 0x0000FFFFFFFFFFFFULL);
    if (!first) f << ",\n";
    f << std::format("  {{\"type_id\":{},\"dbid\":{}}}", type_id, dbid);
    first = false;
  }
  f << "\n]\n";
}

void XmlDatabase::LoadCheckouts() {
  auto path = base_dir_ / "checkouts.json";
  if (!std::filesystem::exists(path)) return;
  auto tree = DataSection::FromJson(path);
  if (!tree) return;

  auto* root = (*tree)->Root();
  for (auto* entry : root->Children()) {
    auto type_id = static_cast<uint16_t>(entry->ReadUint("type_id", 0));
    auto dbid = static_cast<DatabaseID>(entry->ReadUint("dbid", 0));
    if (type_id == 0 || dbid == 0) continue;
    CheckoutInfo info;
    auto ip = entry->ReadUint("base_ip", 0);
    auto port = static_cast<uint16_t>(entry->ReadUint("base_port", 0));
    info.base_addr = Address(ip, port);
    info.app_id = entry->ReadUint("app_id", 0);
    info.entity_id = entry->ReadUint("entity_id", 0);
    checkouts_[CheckoutKey(type_id, dbid)] = info;
  }
}

void XmlDatabase::SaveCheckouts() {
  auto path = base_dir_ / "checkouts.json";
  std::ofstream f(path);
  if (!f) return;

  f << "[\n";
  bool first = true;
  for (const auto& [key, info] : checkouts_) {
    auto type_id = static_cast<uint16_t>(key >> 48);
    auto dbid = static_cast<DatabaseID>(key & 0x0000FFFFFFFFFFFFULL);
    if (!first) f << ",\n";
    f << std::format(
        "  {{\"type_id\":{},\"dbid\":{},\"base_ip\":{},\"base_port\":{}"
        ",\"app_id\":{},\"entity_id\":{}}}",
        type_id, dbid, info.base_addr.Ip(), info.base_addr.Port(), info.app_id, info.entity_id);
    first = false;
  }
  f << "\n]\n";
}

void XmlDatabase::LoadPasswordHashes() {
  password_hashes_.clear();

  auto path = base_dir_ / "password_hashes.json";
  if (!std::filesystem::exists(path)) return;

  auto tree = DataSection::FromJson(path);
  if (!tree) return;

  auto* root = (*tree)->Root();
  for (auto* entry : root->Children()) {
    auto type_id = static_cast<uint16_t>(entry->ReadUint("type_id", 0));
    auto dbid = static_cast<DatabaseID>(entry->ReadUint("dbid", 0));
    auto password_hash = entry->ReadString("password_hash", "");
    if (type_id == 0 || dbid == 0 || password_hash.empty()) continue;
    password_hashes_[CheckoutKey(type_id, dbid)] = std::move(password_hash);
  }
}

void XmlDatabase::SavePasswordHashes() {
  auto path = base_dir_ / "password_hashes.json";
  std::ofstream f(path);
  if (!f) return;

  f << "[\n";
  bool first = true;
  for (const auto& [key, password_hash] : password_hashes_) {
    auto type_id = static_cast<uint16_t>(key >> 48);
    auto dbid = static_cast<DatabaseID>(key & 0x0000FFFFFFFFFFFFULL);
    if (!first) f << ",\n";
    f << std::format("  {{\"type_id\":{},\"dbid\":{},\"password_hash\":\"{}\"}}", type_id, dbid,
                     EscapeJsonString(password_hash));
    first = false;
  }
  f << "\n]\n";
}

auto XmlDatabase::ReadBlob(uint16_t type_id, DatabaseID dbid) const
    -> std::optional<std::vector<std::byte>> {
  auto key = CheckoutKey(type_id, dbid);

  if (auto pending = pending_blob_writes_.find(key); pending != pending_blob_writes_.end()) {
    if (pending->second.deleted) return std::nullopt;
    return pending->second.data;
  }

  if (auto cached = blob_cache_.find(key); cached != blob_cache_.end()) return cached->second;

  auto path = BlobPath(type_id, dbid);
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::nullopt;

  f.seekg(0, std::ios::end);
  auto size = static_cast<size_t>(f.tellg());
  f.seekg(0, std::ios::beg);

  std::vector<std::byte> data(size);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));

  blob_cache_[key] = data;
  return data;
}

void XmlDatabase::WriteBlob(uint16_t type_id, DatabaseID dbid, std::span<const std::byte> data) {
  std::error_code ec;
  std::filesystem::create_directories(TypeDir(type_id), ec);
  std::ofstream f(BlobPath(type_id, dbid), std::ios::binary | std::ios::trunc);
  if (f)
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

void XmlDatabase::DeleteBlob(uint16_t type_id, DatabaseID dbid) {
  std::error_code ec;
  std::filesystem::remove(BlobPath(type_id, dbid), ec);
}

void XmlDatabase::StageBlobWrite(uint16_t type_id, DatabaseID dbid,
                                 std::span<const std::byte> data) {
  auto key = CheckoutKey(type_id, dbid);
  auto& pending = pending_blob_writes_[key];
  if (!pending.deleted) pending_blob_bytes_ -= pending.data.size();
  pending.type_id = type_id;
  pending.dbid = dbid;
  pending.data.assign(data.begin(), data.end());
  pending.deleted = false;
  pending_blob_bytes_ += pending.data.size();
  if (next_flush_deadline_ == TimePoint{}) {
    next_flush_deadline_ = Clock::now() + kFlushInterval;
  }
}

void XmlDatabase::StageBlobDelete(uint16_t type_id, DatabaseID dbid) {
  auto key = CheckoutKey(type_id, dbid);
  auto& pending = pending_blob_writes_[key];
  if (!pending.deleted) pending_blob_bytes_ -= pending.data.size();
  pending.type_id = type_id;
  pending.dbid = dbid;
  pending.data.clear();
  pending.deleted = true;
  blob_cache_.erase(key);
  if (next_flush_deadline_ == TimePoint{}) {
    next_flush_deadline_ = Clock::now() + kFlushInterval;
  }
}

void XmlDatabase::FlushPendingBlobWrites(int budget) {
  auto it = pending_blob_writes_.begin();
  int written = 0;
  while (it != pending_blob_writes_.end() && written < budget) {
    if (!it->second.deleted) pending_blob_bytes_ -= it->second.data.size();
    if (it->second.deleted) {
      DeleteBlob(it->second.type_id, it->second.dbid);
      blob_cache_.erase(it->first);
    } else {
      WriteBlob(it->second.type_id, it->second.dbid, it->second.data);
      blob_cache_[it->first] = std::move(it->second.data);
    }
    it = pending_blob_writes_.erase(it);
    ++written;
  }
}

// ============================================================================
// Private helpers — deferred dispatch
// ============================================================================

void XmlDatabase::FireOrDefer(std::function<void()> cb) {
  if (deferred_mode_) {
    deferred_.push_back(std::move(cb));
  } else {
    cb();
  }
}

}  // namespace atlas
