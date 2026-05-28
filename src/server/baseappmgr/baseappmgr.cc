#include "baseappmgr.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "foundation/clock.h"
#include "foundation/log.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "platform/filesystem.h"
#include "serialization/binary_stream.h"
#include "server/server_app_option.h"
#include "server/snapshot_envelope.h"
#include "server/watcher.h"

namespace atlas {

namespace {

ServerAppOption<uint32_t> s_ha_reattach_watchdog_ms{
    30000u, "baseappmgr_ha_reattach_watchdog_ms",
    "baseappmgr/ha/reattach_watchdog_ms", WatcherMode::kReadWrite};

constexpr uint32_t kSnapshotMagic = 0x424D4731u;  // 'BMG1'
constexpr uint32_t kSnapshotVersion = 1;
// 256 MiB ceiling — BaseAppMgr only persists the BaseApp table,
// next_app_id_, global_bases registry, and dbid_affinity entries
// (no per-cell load buckets). A 4 KiB-per-BaseApp envelope at 65k apps
// is still < 256 MiB, leaving room for global_bases scripts without
// approaching CellAppMgr's 1 GiB cap.
constexpr uint64_t kMaxSnapshotPayloadBytes = 256ull * 1024ull * 1024ull;
constexpr uint64_t kMaxSnapshotFileBytes =
    kMaxSnapshotPayloadBytes + snapshot_envelope::kEnvelopeBytes;
constexpr uint32_t kMaxSnapshotEntries = 1024 * 1024;
constexpr std::size_t kMaxSnapshotStringBytes = 64 * 1024;
constexpr std::string_view kSnapshotModuleName = "BaseAppMgr";
constexpr auto kDirtySnapshotFlushInterval = std::chrono::milliseconds(1000);
constexpr auto kSnapshotSaveWarningThrottle = std::chrono::milliseconds(5000);

using snapshot_envelope::PayloadView;
using snapshot_envelope::FileReadiness;
using snapshot_envelope::WatcherErrorDetail;

auto SnapshotPayload(std::span<const std::byte> bytes) -> Result<PayloadView> {
  return snapshot_envelope::ReadPayload(bytes, kSnapshotMagic, kSnapshotVersion,
                                        kMaxSnapshotPayloadBytes, kSnapshotModuleName);
}

auto SnapshotBackupPath(const std::filesystem::path& path) -> std::filesystem::path {
  return snapshot_envelope::BackupPath(path);
}

auto SnapshotTempPath(const std::filesystem::path& path) -> std::filesystem::path {
  return snapshot_envelope::TempPath(path);
}

auto SnapshotFileReadinessForPath(const std::filesystem::path& path, bool validate)
    -> FileReadiness {
  return snapshot_envelope::Readiness(path, validate, kSnapshotMagic, kSnapshotVersion,
                                      kMaxSnapshotFileBytes, kMaxSnapshotPayloadBytes,
                                      kSnapshotModuleName);
}

auto PreserveSnapshotBackup(const std::filesystem::path& path) -> Result<void> {
  return snapshot_envelope::PreserveBackup(path, kSnapshotMagic, kSnapshotVersion,
                                           kMaxSnapshotPayloadBytes, kSnapshotModuleName);
}

void WriteAddress(BinaryWriter& w, const Address& addr) {
  w.Write(addr.Ip());
  w.Write(addr.Port());
}

auto ReadAddress(BinaryReader& r, const char* context) -> Result<Address> {
  auto ip = r.Read<uint32_t>();
  auto port = r.Read<uint16_t>();
  if (!ip || !port) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("BaseAppMgr snapshot: {} address truncated", context)};
  }
  return Address(*ip, *port);
}

auto ReadCount(BinaryReader& r, const char* context) -> Result<uint32_t> {
  auto count = r.ReadPackedInt();
  if (!count) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("BaseAppMgr snapshot: {} count truncated", context)};
  }
  if (*count > kMaxSnapshotEntries) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("BaseAppMgr snapshot: {} count too large", context)};
  }
  return *count;
}

auto ReadBoundedString(BinaryReader& r, const char* context) -> Result<std::string> {
  auto str = r.ReadString();
  if (!str) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("BaseAppMgr snapshot: {} string truncated", context)};
  }
  if (str->size() > kMaxSnapshotStringBytes) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("BaseAppMgr snapshot: {} string too long", context)};
  }
  return *str;
}

auto AgeMsSince(TimePoint t) -> int64_t {
  if (t.time_since_epoch() == Duration::zero()) return -1;
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
}

}  // namespace

void BaseAppMgr::DbidAffinityTable::Remember(DatabaseID dbid, uint32_t app_id, TimePoint now) {
  if (dbid == kInvalidDBID || app_id == 0) {
    return;
  }

  if (auto existing = entries_.find(dbid); existing != entries_.end()) {
    if (existing->second.app_id != app_id) {
      auto reverse = dbids_by_app_.find(existing->second.app_id);
      if (reverse != dbids_by_app_.end()) {
        reverse->second.erase(dbid);
        if (reverse->second.empty()) {
          dbids_by_app_.erase(reverse);
        }
      }
    }
  }

  entries_[dbid] = Entry{app_id, now};
  dbids_by_app_[app_id].insert(dbid);
}

void BaseAppMgr::DbidAffinityTable::Erase(DatabaseID dbid) {
  auto it = entries_.find(dbid);
  if (it == entries_.end()) {
    return;
  }

  if (auto reverse = dbids_by_app_.find(it->second.app_id); reverse != dbids_by_app_.end()) {
    reverse->second.erase(dbid);
    if (reverse->second.empty()) {
      dbids_by_app_.erase(reverse);
    }
  }

  entries_.erase(it);
}

void BaseAppMgr::DbidAffinityTable::ForgetApp(uint32_t app_id) {
  auto reverse = dbids_by_app_.find(app_id);
  if (reverse == dbids_by_app_.end()) {
    return;
  }

  for (DatabaseID dbid : reverse->second) {
    entries_.erase(dbid);
  }
  dbids_by_app_.erase(reverse);
}

void BaseAppMgr::DbidAffinityTable::PruneExpired(TimePoint now, Duration ttl) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (now - it->second.last_assigned_at <= ttl) {
      ++it;
      continue;
    }

    if (auto reverse = dbids_by_app_.find(it->second.app_id); reverse != dbids_by_app_.end()) {
      reverse->second.erase(it->first);
      if (reverse->second.empty()) {
        dbids_by_app_.erase(reverse);
      }
    }
    it = entries_.erase(it);
  }
}

auto BaseAppMgr::DbidAffinityTable::Find(DatabaseID dbid) const -> std::optional<Entry> {
  auto it = entries_.find(dbid);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto BaseAppMgr::BaseAppInfo::QueuePressure() const -> float {
  const float kPressureUnits =
      static_cast<float>(pending_prepare_count + pending_force_logoff_count + deferred_login_count +
                         logoff_in_flight_count) +
      static_cast<float>(detached_proxy_count) * 0.1f;

  // Queue depth is a balancing hint, not a hard overload signal. Scale it
  // conservatively so transient login bursts spread across BaseApps instead
  // of collapsing the whole cluster into "no_baseapp" rejections.
  return std::min(0.35f, kPressureUnits / 512.0f);
}

auto BaseAppMgr::BaseAppInfo::IsHardOverloaded(float overload_threshold) const -> bool {
  return measured_load >= overload_threshold ||
         pending_prepare_count >= BaseAppMgr::kHardOverloadPendingPrepareLimit ||
         deferred_login_count >= BaseAppMgr::kHardOverloadDeferredLoginLimit ||
         (pending_force_logoff_count + logoff_in_flight_count) >=
             BaseAppMgr::kHardOverloadLogoffLimit;
}

void BaseAppMgr::BaseAppInfo::ApplyLoadReport(
    float load, uint32_t reported_entity_count, uint32_t reported_proxy_count,
    uint32_t reported_pending_prepare_count, uint32_t reported_pending_force_logoff_count,
    uint32_t reported_detached_proxy_count, uint32_t reported_logoff_in_flight_count,
    uint32_t reported_deferred_login_count, TimePoint now) {
  measured_load = std::clamp(load, 0.0f, 1.0f);
  entity_count = reported_entity_count;
  proxy_count = reported_proxy_count;
  pending_prepare_count = reported_pending_prepare_count;
  pending_force_logoff_count = reported_pending_force_logoff_count;
  detached_proxy_count = reported_detached_proxy_count;
  logoff_in_flight_count = reported_logoff_in_flight_count;
  deferred_login_count = reported_deferred_login_count;
  pending_login_allocations = 0;
  effective_load = std::clamp(std::max(measured_load, QueuePressure()), 0.0f, 1.0f);
  last_load_report_at = now;
}

void BaseAppMgr::BaseAppInfo::ReserveLoginSlot(float load_increment) {
  ++pending_login_allocations;
  ++pending_prepare_count;
  ++entity_count;
  ++proxy_count;
  effective_load = std::min(1.0f, std::max(effective_load + load_increment, QueuePressure()));
}

auto BaseAppMgr::BaseAppInfo::HasFreshLoad(TimePoint now, Duration stale_after) const -> bool {
  if (last_load_report_at == TimePoint{}) {
    return true;
  }

  return (now - last_load_report_at) <= stale_after;
}

namespace {

auto ResolveAdvertisedAddr(const Address& advertised, const Address& src) -> Address {
  if (advertised.Ip() != 0) {
    return advertised;
  }
  return Address(src.Ip(), advertised.Port());
}

}  // namespace

auto BaseAppMgr::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);
  BaseAppMgr app(dispatcher, network);
  return app.RunApp(argc, argv);
}

BaseAppMgr::BaseAppMgr(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network) {}

auto BaseAppMgr::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  auto& table = Network().InterfaceTable();

  (void)table.RegisterTypedHandler<baseappmgr::RegisterBaseApp>(
      [this](const Address& src, Channel* ch, const baseappmgr::RegisterBaseApp& msg) {
        OnRegisterBaseapp(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::BaseAppReady>(
      [this](const Address& src, Channel* ch, const baseappmgr::BaseAppReady& msg) {
        OnBaseappReady(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::InformLoad>(
      [this](const Address& src, Channel* ch, const baseappmgr::InformLoad& msg) {
        OnInformLoad(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<login::AllocateBaseApp>(
      [this](const Address& src, Channel* ch, const login::AllocateBaseApp& msg) {
        OnAllocateBaseapp(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::RegisterGlobalBase>(
      [this](const Address& src, Channel* ch, const baseappmgr::RegisterGlobalBase& msg) {
        OnRegisterGlobalBase(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::DeregisterGlobalBase>(
      [this](const Address& src, Channel* ch, const baseappmgr::DeregisterGlobalBase& msg) {
        OnDeregisterGlobalBase(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::HealthProbe>(
      [this](const Address& src, Channel* ch, const baseappmgr::HealthProbe& msg) {
        OnHealthProbe(src, ch, msg);
      });

  // Subscribe to BaseApp death notifications
  GetMachinedClient().Subscribe(
      machined::ListenerType::kDeath, ProcessType::kBaseApp, nullptr,
      [this](const machined::DeathNotification& n) { OnBaseappDeath(n.internal_addr, n.reason); });

  if (!Config().snapshot_path.empty()) {
    auto restore = RestoreSnapshotFromFile(Config().snapshot_path);
    if (!restore) {
      if (restore.Error().Code() == ErrorCode::kNotFound) {
        ATLAS_LOG_INFO("BaseAppMgr: no HA snapshot to restore at {}",
                       Config().snapshot_path.string());
      } else {
        ATLAS_LOG_ERROR("BaseAppMgr: HA snapshot restore failed: {}",
                        restore.Error().Message());
        return false;
      }
    } else {
      ATLAS_LOG_WARNING(
          "BaseAppMgr: restored HA snapshot from {} source={} baseapps={} global_bases={}"
          " dbid_affinity={}",
          Config().snapshot_path.string(), last_snapshot_restore_source_, baseapps_.size(),
          global_bases_.size(), dbid_affinity_.size());
    }
  }

  ATLAS_LOG_INFO("BaseAppMgr: initialised");
  return true;
}

void BaseAppMgr::Fini() {
  if (!Config().snapshot_path.empty() && snapshot_dirty_) {
    SaveConfiguredSnapshot("shutdown");
  }
  ManagerApp::Fini();
}

auto BaseAppMgr::Snapshot() const -> std::vector<std::byte> {
  BinaryWriter payload_writer;
  payload_writer.Write(next_app_id_);

  payload_writer.WritePackedInt(static_cast<uint32_t>(baseapps_.size()));
  for (const auto& [addr, info] : baseapps_) {
    WriteAddress(payload_writer, addr);
    WriteAddress(payload_writer, info.external_addr);
    payload_writer.Write(info.app_id);
    payload_writer.Write<uint8_t>(info.is_ready ? 1u : 0u);
    payload_writer.Write<uint8_t>(info.is_retiring ? 1u : 0u);
  }

  payload_writer.WritePackedInt(static_cast<uint32_t>(global_bases_.size()));
  for (const auto& [key, entry] : global_bases_) {
    payload_writer.WriteString(key);
    WriteAddress(payload_writer, entry.base_addr);
    payload_writer.Write(entry.entity_id);
    payload_writer.Write(entry.type_id);
  }

  const auto& affinity = dbid_affinity_.Entries();
  payload_writer.WritePackedInt(static_cast<uint32_t>(affinity.size()));
  const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          Clock::now().time_since_epoch())
                          .count();
  for (const auto& [dbid, entry] : affinity) {
    payload_writer.Write(static_cast<uint64_t>(dbid));
    payload_writer.Write(entry.app_id);
    const auto entry_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              entry.last_assigned_at.time_since_epoch())
                              .count();
    // Persist age relative to save time so post-restore PruneExpired keeps
    // matching the configured TTL window after a process restart.
    const int64_t age_ns = now_ns - entry_ns;
    payload_writer.Write(age_ns);
  }

  const auto payload = payload_writer.Detach();
  return snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kSnapshotMagic,
      kSnapshotVersion);
}

auto BaseAppMgr::Restore(std::span<const std::byte> bytes) -> Result<void> {
  auto view = SnapshotPayload(bytes);
  if (!view) return view.Error();
  BinaryReader r(view->payload);

  auto next_app = r.Read<uint32_t>();
  if (!next_app) {
    return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: header truncated"};
  }

  std::unordered_map<Address, BaseAppInfo> restored_baseapps;
  std::unordered_map<uint32_t, Address> restored_index;
  std::unordered_set<uint32_t> restored_app_ids;

  auto baseapp_count = ReadCount(r, "baseapps");
  if (!baseapp_count) return baseapp_count.Error();
  for (uint32_t i = 0; i < *baseapp_count; ++i) {
    auto internal = ReadAddress(r, "baseapp");
    auto external = ReadAddress(r, "baseapp_external");
    auto app_id = r.Read<uint32_t>();
    auto ready = r.Read<uint8_t>();
    auto retiring = r.Read<uint8_t>();
    if (!internal || !external || !app_id || !ready || !retiring) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: baseapp truncated"};
    }
    if (*app_id == 0) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: bad baseapp app_id"};
    }
    if (*ready > 1 || *retiring > 1) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: bad baseapp flags"};
    }
    if (!restored_app_ids.insert(*app_id).second) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: duplicate baseapp app_id"};
    }
    BaseAppInfo info;
    info.internal_addr = *internal;
    info.external_addr = *external;
    info.app_id = *app_id;
    info.is_ready = false;  // ready re-asserted only after reattach + InformLoad
    info.is_retiring = *retiring != 0;
    info.channel = nullptr;
    info.needs_reattach = true;
    info.restored_from_snapshot = true;
    info.registered_at = Clock::now();
    restored_baseapps.emplace(*internal, std::move(info));
    restored_index.emplace(*app_id, *internal);
  }

  auto global_count = ReadCount(r, "global_bases");
  if (!global_count) return global_count.Error();
  std::unordered_map<std::string, GlobalBaseEntry> restored_global_bases;
  for (uint32_t i = 0; i < *global_count; ++i) {
    auto key = ReadBoundedString(r, "global_base_key");
    if (!key) return key.Error();
    auto addr = ReadAddress(r, "global_base");
    auto entity_id = r.Read<EntityID>();
    auto type_id = r.Read<uint16_t>();
    if (!addr || !entity_id || !type_id) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: global base truncated"};
    }
    GlobalBaseEntry entry;
    entry.key = *key;
    entry.base_addr = *addr;
    entry.entity_id = *entity_id;
    entry.type_id = *type_id;
    if (!restored_global_bases.emplace(*key, std::move(entry)).second) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: duplicate global base"};
    }
  }

  auto affinity_count = ReadCount(r, "dbid_affinity");
  if (!affinity_count) return affinity_count.Error();
  struct AffinityEntry {
    DatabaseID dbid;
    uint32_t app_id;
    int64_t age_ns;
  };
  std::vector<AffinityEntry> restored_affinity;
  restored_affinity.reserve(*affinity_count);
  for (uint32_t i = 0; i < *affinity_count; ++i) {
    auto dbid = r.Read<uint64_t>();
    auto app_id = r.Read<uint32_t>();
    auto age_ns = r.Read<int64_t>();
    if (!dbid || !app_id || !age_ns) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: affinity truncated"};
    }
    if (*app_id != 0 && !restored_app_ids.contains(*app_id)) {
      // affinity references a BaseApp that the snapshot itself does not
      // describe — drop it; the entry would be stale anyway.
      continue;
    }
    restored_affinity.push_back({static_cast<DatabaseID>(*dbid), *app_id, *age_ns});
  }

  if (r.Remaining() != 0) {
    return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: trailing bytes"};
  }

  baseapps_ = std::move(restored_baseapps);
  app_id_index_ = std::move(restored_index);
  global_bases_ = std::move(restored_global_bases);
  next_app_id_ = *next_app;

  dbid_affinity_.Clear();
  const auto now = Clock::now();
  for (const auto& entry : restored_affinity) {
    const auto restored_tp =
        now - std::chrono::duration_cast<Duration>(std::chrono::nanoseconds(entry.age_ns));
    dbid_affinity_.Remember(entry.dbid, entry.app_id, restored_tp);
  }
  return {};
}

auto BaseAppMgr::SaveSnapshotToFile(const std::filesystem::path& path) -> Result<void> {
  if (path.empty()) return {};
  last_snapshot_attempt_at_ = Clock::now();
  auto record_error = [this, &path](const Error& error) -> Error {
    ++snapshot_save_failure_count_;
    ++snapshot_failure_count_;
    last_snapshot_save_path_ = path;
    last_snapshot_save_error_ = std::string(error.Message());
    return error;
  };
  if (auto parent = path.parent_path(); !parent.empty()) {
    auto create = fs::CreateDirectories(parent);
    if (!create) return record_error(create.Error());
  }

  const auto bytes = Snapshot();
  if (bytes.size() > kMaxSnapshotFileBytes) {
    return record_error(
        Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: file too large"});
  }
  if (auto backup = PreserveSnapshotBackup(path); !backup) {
    if (backup.Error().Code() != ErrorCode::kInvalidArgument) {
      return record_error(backup.Error());
    }
    ++snapshot_backup_skip_count_;
    ATLAS_LOG_WARNING("BaseAppMgr: HA snapshot backup skipped (.bak lags new main): {}",
                      backup.Error().Message());
  }
  auto tmp = SnapshotTempPath(path);
  auto write = fs::WriteFile(tmp, bytes);
  if (!write) {
    (void)fs::RemoveFile(tmp);
    return record_error(write.Error());
  }
  auto replace = fs::AtomicReplaceFile(tmp, path);
  if (!replace) {
    (void)fs::RemoveFile(tmp);
    return record_error(replace.Error());
  }
  last_snapshot_bytes_ = bytes.size();
  last_snapshot_save_at_ = Clock::now();
  last_snapshot_save_path_ = path;
  last_snapshot_save_error_.clear();
  snapshot_dirty_ = false;
  snapshot_dirty_at_ = {};
  snapshot_dirty_reason_.clear();
  ++snapshot_save_count_;

  // High-water warning — see CellAppMgr for the parallel implementation
  // and snapshot_envelope::EvaluateSizeWarning for the decision logic
  // (both mgrs route through the same helper).
  const auto now = Clock::now();
  const auto decision =
      snapshot_envelope::EvaluateSizeWarning(SnapshotSizeHighWaterPct(), now,
                                             last_snapshot_size_warning_at_);
  if (decision.should_log) {
    last_snapshot_size_warning_at_ = now;
    ATLAS_LOG_WARNING(
        "BaseAppMgr: HA snapshot file size at {}% of {} byte ceiling ({} bytes) — "
        "save will start rejecting writes once it crosses the ceiling",
        SnapshotSizeHighWaterPct(), kMaxSnapshotFileBytes, bytes.size());
  } else if (decision.should_reset) {
    last_snapshot_size_warning_at_ = {};
  }
  return {};
}

auto BaseAppMgr::RestoreSnapshotFromFile(const std::filesystem::path& path) -> Result<void> {
  if (path.empty()) return {};
  last_snapshot_restore_attempt_at_ = Clock::now();

  auto restore_one = [this](const std::filesystem::path& candidate) -> Result<void> {
    if (!fs::Exists(candidate)) {
      return Error{ErrorCode::kNotFound,
                   std::format("snapshot file not found: {}", candidate.string())};
    }
    auto size = fs::FileSize(candidate);
    if (!size) return size.Error();
    if (*size > kMaxSnapshotFileBytes) {
      return Error{ErrorCode::kInvalidArgument, "BaseAppMgr snapshot: file too large"};
    }
    auto bytes = fs::ReadFile(candidate);
    if (!bytes) return bytes.Error();
    auto restore = Restore(std::span<const std::byte>(bytes->data(), bytes->size()));
    if (!restore) return restore;
    ++snapshot_restore_count_;
    last_snapshot_bytes_ = bytes->size();
    last_snapshot_restore_at_ = Clock::now();
    return {};
  };

  auto primary = restore_one(path);
  if (primary) {
    last_snapshot_restore_source_ = "primary";
    last_snapshot_restore_path_ = path;
    last_snapshot_restore_error_.clear();
    last_snapshot_restore_primary_error_.clear();
    return {};
  }

  const auto backup_path = SnapshotBackupPath(path);
  auto backup = restore_one(backup_path);
  if (backup) {
    ++snapshot_fallback_restore_count_;
    last_snapshot_restore_source_ = "backup";
    last_snapshot_restore_path_ = backup_path;
    last_snapshot_restore_error_.clear();
    last_snapshot_restore_primary_error_ = primary.Error().Message();
    ATLAS_LOG_WARNING("BaseAppMgr: restored HA snapshot backup {} after primary failed: {}",
                      backup_path.string(), primary.Error().Message());
    return {};
  }
  if (primary.Error().Code() == ErrorCode::kNotFound &&
      backup.Error().Code() == ErrorCode::kNotFound) {
    last_snapshot_restore_source_ = "none";
    last_snapshot_restore_path_.clear();
    last_snapshot_restore_error_.clear();
    last_snapshot_restore_primary_error_.clear();
    return primary.Error();
  }
  ++snapshot_restore_failure_count_;
  ++snapshot_failure_count_;
  last_snapshot_restore_source_ = "none";
  last_snapshot_restore_path_.clear();
  last_snapshot_restore_primary_error_ = primary.Error().Message();
  if (primary.Error().Code() == ErrorCode::kNotFound) {
    last_snapshot_restore_error_ = backup.Error().Message();
    return backup.Error();
  }
  if (backup.Error().Code() == ErrorCode::kNotFound) {
    last_snapshot_restore_error_ = primary.Error().Message();
    return primary.Error();
  }
  auto error = Error{primary.Error().Code(),
                     std::format("{}; backup failed: {}", primary.Error().Message(),
                                 backup.Error().Message())};
  last_snapshot_restore_error_ = error.Message();
  return error;
}

void BaseAppMgr::OnTickComplete() {
  ManagerApp::OnTickComplete();
  AuditReattachWatchdog();
  if (Config().snapshot_path.empty() || Config().snapshot_interval_ms <= 0) return;
  const auto now = Clock::now();
  const auto interval =
      std::chrono::duration_cast<Duration>(std::chrono::milliseconds(Config().snapshot_interval_ms));
  const auto dirty_interval =
      std::min(interval, std::chrono::duration_cast<Duration>(kDirtySnapshotFlushInterval));
  const bool never_attempted =
      last_snapshot_attempt_at_.time_since_epoch() == Duration::zero();
  const auto since_attempt = now - last_snapshot_attempt_at_;
  if (snapshot_dirty_ && (never_attempted || since_attempt >= dirty_interval)) {
    SaveConfiguredSnapshot("dirty");
  } else if (never_attempted || since_attempt >= interval) {
    SaveConfiguredSnapshot("periodic");
  }
}

void BaseAppMgr::MarkSnapshotDirty(const char* reason) {
  if (!snapshot_dirty_) snapshot_dirty_at_ = Clock::now();
  snapshot_dirty_ = true;
  snapshot_dirty_reason_ = reason == nullptr ? "unknown" : reason;
}

void BaseAppMgr::SaveConfiguredSnapshot(const char* context) {
  if (Config().snapshot_path.empty()) return;
  auto save = SaveSnapshotToFile(Config().snapshot_path);
  if (save) {
    last_snapshot_save_warning_at_ = {};
    return;
  }
  const auto now = Clock::now();
  const auto throttle =
      std::chrono::duration_cast<Duration>(kSnapshotSaveWarningThrottle);
  if (last_snapshot_save_warning_at_ != TimePoint{} &&
      now - last_snapshot_save_warning_at_ < throttle) {
    return;
  }
  last_snapshot_save_warning_at_ = now;
  ATLAS_LOG_WARNING("BaseAppMgr: HA snapshot {} save failed: {}", context,
                    save.Error().Message());
}

auto BaseAppMgr::SnapshotFilePathForWatcher() const -> std::string {
  const auto& configured = Config().snapshot_path;
  const auto& base_path = configured.empty() ? last_snapshot_save_path_ : configured;
  return base_path.string();
}

auto BaseAppMgr::SnapshotFilePresentForWatcher() const -> bool {
  return SnapshotFileReadinessForPath(SnapshotFilePathForWatcher(), false).present;
}

auto BaseAppMgr::SnapshotFileBytesForWatcher() const -> uint64_t {
  return SnapshotFileReadinessForPath(SnapshotFilePathForWatcher(), false).bytes;
}

auto BaseAppMgr::BuildSnapshotFileStatusSummary() const -> std::string {
  const auto path = SnapshotFilePathForWatcher();
  const auto readiness = SnapshotFileReadinessForPath(path, true);
  return std::format(
      "state={} path={} present={} bytes={} valid={} error_present={} error_detail={}",
      readiness.state, path, readiness.present ? 1 : 0, readiness.bytes,
      readiness.valid ? 1 : 0, readiness.error_present ? 1 : 0, readiness.error_detail);
}

auto BaseAppMgr::SnapshotBackupPathForWatcher() const -> std::string {
  const auto base = SnapshotFilePathForWatcher();
  if (base.empty()) return "";
  return SnapshotBackupPath(base).string();
}

auto BaseAppMgr::SnapshotBackupPresentForWatcher() const -> bool {
  return SnapshotFileReadinessForPath(SnapshotBackupPathForWatcher(), false).present;
}

auto BaseAppMgr::SnapshotBackupBytesForWatcher() const -> uint64_t {
  return SnapshotFileReadinessForPath(SnapshotBackupPathForWatcher(), false).bytes;
}

auto BaseAppMgr::BuildSnapshotBackupStatusSummary() const -> std::string {
  const auto path = SnapshotBackupPathForWatcher();
  const auto readiness = SnapshotFileReadinessForPath(path, true);
  return std::format(
      "state={} path={} present={} bytes={} valid={} error_present={} error_detail={}",
      readiness.state, path, readiness.present ? 1 : 0, readiness.bytes,
      readiness.valid ? 1 : 0, readiness.error_present ? 1 : 0, readiness.error_detail);
}

auto BaseAppMgr::LastSnapshotAttemptAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_attempt_at_);
}

auto BaseAppMgr::LastSnapshotSaveAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_save_at_);
}

auto BaseAppMgr::LastSnapshotDirtyAgeMsForWatcher() const -> int64_t {
  return snapshot_dirty_ ? AgeMsSince(snapshot_dirty_at_) : -1;
}

auto BaseAppMgr::LastSnapshotRestoreAttemptAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_restore_attempt_at_);
}

auto BaseAppMgr::LastSnapshotRestoreAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_restore_at_);
}

auto BaseAppMgr::SnapshotSaveStaleForWatcher() const -> bool {
  if (Config().snapshot_path.empty() || Config().snapshot_interval_ms <= 0) return false;
  const bool attempted = last_snapshot_attempt_at_.time_since_epoch() != Duration::zero();
  const bool saved = last_snapshot_save_at_.time_since_epoch() != Duration::zero();
  if (!saved) return attempted;
  return LastSnapshotSaveAgeMsForWatcher() >
         static_cast<int64_t>(Config().snapshot_interval_ms) * 2;
}

auto BaseAppMgr::SnapshotSizeHighWaterPct() const -> uint32_t {
  if (last_snapshot_bytes_ == 0) return 0;
  const auto pct = (static_cast<uint64_t>(last_snapshot_bytes_) * 100u) / kMaxSnapshotFileBytes;
  return static_cast<uint32_t>(std::min<uint64_t>(pct, 100));
}

auto BaseAppMgr::BuildSnapshotStatusSummary() const -> std::string {
  const char* state = "healthy";
  if (Config().snapshot_path.empty()) state = "disabled";
  else if (Config().snapshot_interval_ms <= 0) state = "disabled";
  else if (SnapshotSaveStaleForWatcher()) state = "stale";
  else if (snapshot_failure_count_ > 0) state = "degraded";
  const std::string error_detail = last_snapshot_save_error_.empty() ?
      std::string{"none"} : WatcherErrorDetail(last_snapshot_save_error_);
  return std::format(
      "state={} configured={} interval_ms={} saves={} save_failures={} restore_failures={}"
      " failures={} backup_skips={} stale={} last_attempt_age_ms={} last_save_age_ms={}"
      " bytes={} dirty={} dirty_age_ms={} dirty_reason={} error_present={} error_detail={}",
      state, Config().snapshot_path.empty() ? 0 : 1, Config().snapshot_interval_ms,
      snapshot_save_count_, snapshot_save_failure_count_, snapshot_restore_failure_count_,
      snapshot_failure_count_, snapshot_backup_skip_count_,
      SnapshotSaveStaleForWatcher() ? 1 : 0, LastSnapshotAttemptAgeMsForWatcher(),
      LastSnapshotSaveAgeMsForWatcher(), last_snapshot_bytes_, snapshot_dirty_ ? 1 : 0,
      LastSnapshotDirtyAgeMsForWatcher(),
      snapshot_dirty_ ? snapshot_dirty_reason_ : std::string{"none"},
      last_snapshot_save_error_.empty() ? 0 : 1, error_detail);
}

auto BaseAppMgr::ReattachWatchdogWindow() const -> Duration {
  return std::chrono::duration_cast<Duration>(
      std::chrono::milliseconds(s_ha_reattach_watchdog_ms.Value()));
}

auto BaseAppMgr::IsReattachStuck(const BaseAppInfo& info, TimePoint now) const -> bool {
  if (!info.restored_from_snapshot || !info.needs_reattach) return false;
  return now - info.registered_at >= ReattachWatchdogWindow();
}

void BaseAppMgr::AuditReattachWatchdog() {
  if (baseapps_.empty()) return;
  const auto now = Clock::now();
  const auto window = ReattachWatchdogWindow();
  const auto min_repeat = std::chrono::duration_cast<Duration>(std::chrono::milliseconds(1000));
  const auto repeat_window = window < min_repeat ? min_repeat : window;
  for (auto& [_, info] : baseapps_) {
    if (!IsReattachStuck(info, now)) continue;
    if (info.last_reattach_watchdog_log_at != TimePoint{} &&
        now - info.last_reattach_watchdog_log_at < repeat_window) {
      continue;
    }
    ATLAS_LOG_WARNING("BaseAppMgr: restored BaseApp reattach stuck app_id={} addr={}:{} age_ms={}",
                      info.app_id, info.internal_addr.Ip(), info.internal_addr.Port(),
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          now - info.registered_at).count());
    info.last_reattach_watchdog_log_at = now;
  }
}

auto BaseAppMgr::RestoredBaseAppCount() const -> std::size_t {
  return static_cast<std::size_t>(std::count_if(
      baseapps_.begin(), baseapps_.end(),
      [](const auto& entry) { return entry.second.restored_from_snapshot; }));
}

auto BaseAppMgr::PendingReattachBaseAppCount() const -> std::size_t {
  return static_cast<std::size_t>(std::count_if(
      baseapps_.begin(), baseapps_.end(), [](const auto& entry) {
        return entry.second.restored_from_snapshot && entry.second.needs_reattach;
      }));
}

auto BaseAppMgr::CompletedReattachBaseAppCount() const -> std::size_t {
  return RestoredBaseAppCount() - PendingReattachBaseAppCount();
}

auto BaseAppMgr::StuckReattachBaseAppCount() const -> std::size_t {
  const auto now = Clock::now();
  std::size_t stuck = 0;
  for (const auto& [_, info] : baseapps_) {
    if (IsReattachStuck(info, now)) ++stuck;
  }
  return stuck;
}

auto BaseAppMgr::ReattachCompleted() const -> bool {
  return PendingReattachBaseAppCount() == 0;
}

auto BaseAppMgr::ReattachStateForWatcher() const -> std::string {
  if (RestoredBaseAppCount() == 0) return "idle";
  if (StuckReattachBaseAppCount() > 0) return "stuck";
  if (PendingReattachBaseAppCount() > 0) return "pending";
  return "complete";
}

auto BaseAppMgr::BuildReattachStatusSummary() const -> std::string {
  const auto state = ReattachStateForWatcher();
  const auto restored = RestoredBaseAppCount();
  const auto pending = PendingReattachBaseAppCount();
  const auto stuck = StuckReattachBaseAppCount();
  const auto completed = restored - pending;
  std::string out = std::format(
      "state={} restored={} pending={} stuck={} completed={} completed_count={}",
      state, restored, pending, stuck, ReattachCompleted() ? 1 : 0, completed);
  for (const auto& [_, info] : baseapps_) {
    if (!info.restored_from_snapshot) continue;
    const char* host_state = info.needs_reattach ? (IsReattachStuck(info, Clock::now()) ? "stuck"
                                                                                        : "pending")
                                                  : "attached";
    out += std::format(" app={} addr={}:{} state={}", info.app_id, info.internal_addr.Ip(),
                       info.internal_addr.Port(), host_state);
  }
  return out;
}

auto BaseAppMgr::BuildSnapshotRestoreStatusSummary() const -> std::string {
  const char* state = last_snapshot_restore_source_.empty() ? "idle"
                          : last_snapshot_restore_source_ == "none" ?
                              (snapshot_restore_failure_count_ > 0 ? "failed" : "idle")
                              : "ready";
  const std::string error_detail = last_snapshot_restore_error_.empty() ?
      std::string{"none"} : WatcherErrorDetail(last_snapshot_restore_error_);
  const std::string primary_detail = last_snapshot_restore_primary_error_.empty() ?
      std::string{"none"} : WatcherErrorDetail(last_snapshot_restore_primary_error_);
  return std::format(
      "state={} source={} restores={} fallback_restores={} restore_failures={}"
      " failures={} last_attempt_age_ms={} last_restore_age_ms={} error_present={}"
      " error_detail={} primary_error_present={} primary_error_detail={}",
      state, last_snapshot_restore_source_, snapshot_restore_count_,
      snapshot_fallback_restore_count_, snapshot_restore_failure_count_,
      snapshot_failure_count_, LastSnapshotRestoreAttemptAgeMsForWatcher(),
      LastSnapshotRestoreAgeMsForWatcher(),
      last_snapshot_restore_error_.empty() ? 0 : 1, error_detail,
      last_snapshot_restore_primary_error_.empty() ? 0 : 1, primary_detail);
}

void BaseAppMgr::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<std::size_t>("baseappmgr/baseapp_count",
                      std::function<std::size_t()>([this] { return baseapps_.size(); }));
  wr.Add<std::size_t>("baseappmgr/global_base_count",
                      std::function<std::size_t()>([this] { return global_bases_.size(); }));
  wr.Add<std::size_t>("baseappmgr/dbid_affinity_count",
                      std::function<std::size_t()>([this] { return dbid_affinity_.size(); }));

  wr.Add<std::string>("baseappmgr/ha/snapshot_path",
                      std::function<std::string()>(
                          [this] { return SnapshotFilePathForWatcher(); }));
  wr.Add<int>("baseappmgr/ha/snapshot_interval_ms",
              std::function<int()>([this] { return Config().snapshot_interval_ms; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_bytes",
                   std::function<uint64_t()>(
                       [this] { return static_cast<uint64_t>(last_snapshot_bytes_); }));
  wr.Add<bool>("baseappmgr/ha/snapshot_file_present",
               std::function<bool()>(
                   [this] { return SnapshotFilePresentForWatcher(); }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_file_bytes",
                   std::function<uint64_t()>(
                       [this] { return SnapshotFileBytesForWatcher(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_file_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotFileStatusSummary(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_backup_path",
                      std::function<std::string()>(
                          [this] { return SnapshotBackupPathForWatcher(); }));
  wr.Add<bool>("baseappmgr/ha/snapshot_backup_present",
               std::function<bool()>(
                   [this] { return SnapshotBackupPresentForWatcher(); }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_backup_bytes",
                   std::function<uint64_t()>(
                       [this] { return SnapshotBackupBytesForWatcher(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_backup_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotBackupStatusSummary(); }));
  wr.Add<int64_t>("baseappmgr/ha/snapshot_last_save_attempt_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotAttemptAgeMsForWatcher(); }));
  wr.Add<int64_t>("baseappmgr/ha/snapshot_last_save_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotSaveAgeMsForWatcher(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_last_save_path",
                      std::function<std::string()>(
                          [this] { return last_snapshot_save_path_.string(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_last_save_error",
                      std::function<std::string()>(
                          [this] { return last_snapshot_save_error_; }));
  wr.Add<bool>("baseappmgr/ha/snapshot_dirty",
               std::function<bool()>([this] { return snapshot_dirty_; }));
  wr.Add<int64_t>("baseappmgr/ha/snapshot_dirty_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotDirtyAgeMsForWatcher(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_dirty_reason",
                      std::function<std::string()>([this] {
                        return snapshot_dirty_ ? snapshot_dirty_reason_ : std::string{};
                      }));
  wr.Add<bool>("baseappmgr/ha/snapshot_save_stale",
               std::function<bool()>(
                   [this] { return SnapshotSaveStaleForWatcher(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotStatusSummary(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_last_restore_source",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_source_; }));
  wr.Add<int64_t>("baseappmgr/ha/snapshot_last_restore_attempt_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotRestoreAttemptAgeMsForWatcher(); }));
  wr.Add<int64_t>("baseappmgr/ha/snapshot_last_restore_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotRestoreAgeMsForWatcher(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_last_restore_path",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_path_.string(); }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_last_restore_error",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_error_; }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_last_restore_primary_error",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_primary_error_; }));
  wr.Add<std::string>("baseappmgr/ha/snapshot_restore_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotRestoreStatusSummary(); }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_saves",
                   std::function<uint64_t()>(
                       [this] { return snapshot_save_count_; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_restores",
                   std::function<uint64_t()>(
                       [this] { return snapshot_restore_count_; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_fallback_restores",
                   std::function<uint64_t()>(
                       [this] { return snapshot_fallback_restore_count_; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_save_failures",
                   std::function<uint64_t()>(
                       [this] { return snapshot_save_failure_count_; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_restore_failures",
                   std::function<uint64_t()>(
                       [this] { return snapshot_restore_failure_count_; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_failures",
                   std::function<uint64_t()>(
                       [this] { return snapshot_failure_count_; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_backup_skips",
                   std::function<uint64_t()>(
                       [this] { return snapshot_backup_skip_count_; }));
  wr.Add<uint64_t>("baseappmgr/ha/snapshot_max_bytes",
                   std::function<uint64_t()>(
                       [] { return kMaxSnapshotFileBytes; }));
  wr.Add<uint32_t>("baseappmgr/ha/snapshot_size_high_water_pct",
                   std::function<uint32_t()>(
                       [this] { return SnapshotSizeHighWaterPct(); }));

  // reattach state. reattach_watchdog_ms is registered as a ReadWrite
  // ServerAppOption (above), so verify scripts can shrink the window via
  // atlas_tool set-watch without rebuilding.
  wr.Add<std::size_t>("baseappmgr/ha/restored_baseapps",
                      std::function<std::size_t()>(
                          [this] { return RestoredBaseAppCount(); }));
  wr.Add<std::size_t>("baseappmgr/ha/reattach_pending",
                      std::function<std::size_t()>(
                          [this] { return PendingReattachBaseAppCount(); }));
  wr.Add<std::size_t>("baseappmgr/ha/reattach_completed_count",
                      std::function<std::size_t()>(
                          [this] { return CompletedReattachBaseAppCount(); }));
  wr.Add<bool>("baseappmgr/ha/reattach_completed",
               std::function<bool()>([this] { return ReattachCompleted(); }));
  wr.Add<std::size_t>("baseappmgr/ha/reattach_stuck",
                      std::function<std::size_t()>(
                          [this] { return StuckReattachBaseAppCount(); }));
  wr.Add<std::string>("baseappmgr/ha/reattach_state",
                      std::function<std::string()>(
                          [this] { return ReattachStateForWatcher(); }));
  wr.Add<std::string>("baseappmgr/ha/reattach_status",
                      std::function<std::string()>(
                          [this] { return BuildReattachStatusSummary(); }));
}

void BaseAppMgr::OnRegisterBaseapp(const Address& src, Channel* ch,
                                   const baseappmgr::RegisterBaseApp& msg) {
  const Address kInternalAddr = ResolveAdvertisedAddr(msg.internal_addr, src);
  const Address kExternalAddr = ResolveAdvertisedAddr(msg.external_addr, src);

  if (auto existing_it = baseapps_.find(kInternalAddr); existing_it != baseapps_.end()) {
    auto& existing = existing_it->second;
    if (!existing.needs_reattach) {
      ATLAS_LOG_WARNING("BaseAppMgr: duplicate BaseApp registration for internal addr {}:{}",
                        kInternalAddr.Ip(), kInternalAddr.Port());
      baseappmgr::RegisterBaseAppAck ack;
      ack.success = false;
      (void)ch->SendMessage(ack);
      return;
    }
    // Snapshot-restored entry: the surviving BaseApp is reconnecting, keep
    // its app_id and replay nothing on this side — BaseApp re-asserts ready
    // via BaseAppReady. restored_from_snapshot stays sticky for watcher
    // semantics (cleared only at OnBaseappDeath).
    existing.channel = ch;
    existing.external_addr = kExternalAddr;
    existing.needs_reattach = false;
    existing.is_ready = false;
    existing.registered_at = Clock::now();
    existing.last_reattach_watchdog_log_at = {};
    MarkSnapshotDirty("baseapp-reattach");
    baseappmgr::RegisterBaseAppAck ack;
    ack.success = true;
    ack.app_id = existing.app_id;
    ack.game_time = GameTime();
    (void)ch->SendMessage(ack);
    ATLAS_LOG_INFO("BaseAppMgr: BaseApp reattached app_id={} internal={}:{} external={}:{}",
                   existing.app_id, kInternalAddr.Ip(), kInternalAddr.Port(),
                   kExternalAddr.Ip(), kExternalAddr.Port());
    return;
  }

  uint32_t app_id = next_app_id_++;
  BaseAppInfo info;
  info.internal_addr = kInternalAddr;
  info.external_addr = kExternalAddr;
  info.app_id = app_id;
  info.channel = ch;
  info.registered_at = Clock::now();
  const auto [it, inserted] = baseapps_.emplace(kInternalAddr, std::move(info));
  if (!inserted) {
    ATLAS_LOG_ERROR("BaseAppMgr: failed to insert BaseApp registration for {}:{}",
                    kInternalAddr.Ip(), kInternalAddr.Port());
    baseappmgr::RegisterBaseAppAck ack;
    ack.success = false;
    (void)ch->SendMessage(ack);
    return;
  }
  app_id_index_.emplace(app_id, it->first);
  MarkSnapshotDirty("baseapp-register");

  baseappmgr::RegisterBaseAppAck ack;
  ack.success = true;
  ack.app_id = app_id;
  ack.game_time = GameTime();
  (void)ch->SendMessage(ack);

  ATLAS_LOG_INFO("BaseAppMgr: BaseApp registered app_id={} internal={}:{} external={}:{}", app_id,
                 kInternalAddr.Ip(), kInternalAddr.Port(), kExternalAddr.Ip(),
                 kExternalAddr.Port());
}

void BaseAppMgr::OnBaseappReady(const Address& src, Channel* ch,
                                const baseappmgr::BaseAppReady& msg) {
  BaseAppInfo* info = FindBaseappByAppId(msg.app_id);
  if (info == nullptr) {
    ATLAS_LOG_WARNING("BaseAppMgr: BaseAppReady for unknown app_id={} from {}:{}", msg.app_id,
                      src.Ip(), src.Port());
    return;
  }

  if (!MatchesRegisteredSource(*info, src, ch, "BaseAppReady")) return;

  if (!info->is_ready) {
    info->is_ready = true;
    MarkSnapshotDirty("baseapp-ready");
  }
  ATLAS_LOG_INFO("BaseAppMgr: BaseApp app_id={} is ready", msg.app_id);
}

void BaseAppMgr::OnHealthProbe(const Address&, Channel* ch,
                               const baseappmgr::HealthProbe& msg) {
  if (ch == nullptr) return;
  baseappmgr::HealthProbeAck ack;
  ack.nonce = msg.nonce;
  ack.game_time = GameTime();
  ack.snapshot_saves = snapshot_save_count_;
  ack.snapshot_failures = snapshot_failure_count_;
  ack.snapshot_dirty = snapshot_dirty_;
  ack.snapshot_save_stale = SnapshotSaveStaleForWatcher();
  (void)ch->SendMessage(ack);
}

void BaseAppMgr::OnInformLoad(const Address& src, Channel* ch, const baseappmgr::InformLoad& msg) {
  BaseAppInfo* info = FindBaseappByAppId(msg.app_id);
  if (info == nullptr) {
    ATLAS_LOG_WARNING("BaseAppMgr: InformLoad for unknown app_id={} from {}:{}", msg.app_id,
                      src.Ip(), src.Port());
    return;
  }

  if (!MatchesRegisteredSource(*info, src, ch, "InformLoad")) return;

  info->ApplyLoadReport(msg.load, msg.entity_count, msg.proxy_count, msg.pending_prepare_count,
                        msg.pending_force_logoff_count, msg.detached_proxy_count,
                        msg.logoff_in_flight_count, msg.deferred_login_count, Clock::now());
}

void BaseAppMgr::OnAllocateBaseapp(const Address& src, Channel* ch,
                                   const login::AllocateBaseApp& msg) {
  ATLAS_LOG_DEBUG("BaseAppMgr: allocate request_id={} type_id={} dbid={} from {}:{}",
                  msg.request_id, msg.type_id, msg.dbid, src.Ip(), src.Port());
  login::AllocateBaseAppResult result;
  result.request_id = msg.request_id;

  if (IsOverloaded()) {
    ATLAS_LOG_WARNING("BaseAppMgr: overloaded, rejecting AllocateBaseApp req={}", msg.request_id);
    result.success = false;
    (void)ch->SendMessage(result);
    return;
  }

  auto* best = FindAllocationTarget(msg.dbid);
  if (!best) {
    ATLAS_LOG_WARNING("BaseAppMgr: no ready BaseApp available for req={}", msg.request_id);
    result.success = false;
    (void)ch->SendMessage(result);
    return;
  }

  result.success = true;
  result.internal_addr = best->internal_addr;
  result.external_addr = best->external_addr;
  const auto kSendResult = ch->SendMessage(result);
  if (!kSendResult) {
    ATLAS_LOG_WARNING("BaseAppMgr: failed to reply AllocateBaseApp req={} app_id={}: {}",
                      msg.request_id, best->app_id, kSendResult.Error().Message());
    return;
  }

  RecordSuccessfulAllocation(best->app_id, msg.dbid, Clock::now());

  ATLAS_LOG_DEBUG("BaseAppMgr: allocated BaseApp app_id={} for req={} dbid={}", best->app_id,
                  msg.request_id, msg.dbid);
  (void)src;
}

void BaseAppMgr::OnRegisterGlobalBase(const Address& src, Channel* /*ch*/,
                                      const baseappmgr::RegisterGlobalBase& msg) {
  GlobalBaseEntry entry;
  entry.key = msg.key;
  entry.base_addr = src;
  entry.entity_id = msg.entity_id;
  entry.type_id = msg.type_id;
  global_bases_[msg.key] = std::move(entry);
  MarkSnapshotDirty("global-base-register");

  baseappmgr::GlobalBaseNotification notif;
  notif.key = msg.key;
  notif.base_addr = src;
  notif.entity_id = msg.entity_id;
  notif.type_id = msg.type_id;
  notif.added = true;
  BroadcastToAllBaseapps(notif);

  ATLAS_LOG_INFO("BaseAppMgr: global base '{}' registered entity={}", msg.key, msg.entity_id);
}

void BaseAppMgr::OnDeregisterGlobalBase(const Address& /*src*/, Channel* /*ch*/,
                                        const baseappmgr::DeregisterGlobalBase& msg) {
  auto it = global_bases_.find(msg.key);
  if (it == global_bases_.end()) return;

  baseappmgr::GlobalBaseNotification notif;
  notif.key = msg.key;
  notif.base_addr = it->second.base_addr;
  notif.entity_id = it->second.entity_id;
  notif.type_id = it->second.type_id;
  notif.added = false;
  global_bases_.erase(it);
  MarkSnapshotDirty("global-base-deregister");
  BroadcastToAllBaseapps(notif);

  ATLAS_LOG_INFO("BaseAppMgr: global base '{}' deregistered", msg.key);
}

auto BaseAppMgr::FindBaseappByAppId(uint32_t app_id) -> BaseAppInfo* {
  const auto kIndexIt = app_id_index_.find(app_id);
  if (kIndexIt == app_id_index_.end()) return nullptr;

  const auto kIt = baseapps_.find(kIndexIt->second);
  if (kIt == baseapps_.end()) {
    app_id_index_.erase(kIndexIt);
    return nullptr;
  }

  return &kIt->second;
}

auto BaseAppMgr::FindBaseappByAppId(uint32_t app_id) const -> const BaseAppInfo* {
  const auto kIndexIt = app_id_index_.find(app_id);
  if (kIndexIt == app_id_index_.end()) return nullptr;

  const auto kIt = baseapps_.find(kIndexIt->second);
  return (kIt != baseapps_.end()) ? &kIt->second : nullptr;
}

auto BaseAppMgr::MatchesRegisteredSource(const BaseAppInfo& info, const Address& src,
                                         const Channel* ch, std::string_view operation) const
    -> bool {
  if (src != info.internal_addr) {
    ATLAS_LOG_WARNING("BaseAppMgr: {} source mismatch for app_id={} expected {}:{} got {}:{}",
                      operation, info.app_id, info.internal_addr.Ip(), info.internal_addr.Port(),
                      src.Ip(), src.Port());
    return false;
  }

  if (ch != nullptr && info.channel != nullptr && info.channel != ch) {
    ATLAS_LOG_WARNING("BaseAppMgr: {} channel mismatch for app_id={}", operation, info.app_id);
    return false;
  }

  return true;
}

auto BaseAppMgr::IsAllocationCandidate(const BaseAppInfo& info, TimePoint now,
                                       Duration stale_after) const -> bool {
  // needs_reattach hosts came from a snapshot Restore and haven't reconnected;
  // routing new clients to them would land on a dead address.
  if (info.needs_reattach) return false;
  return info.is_ready && !info.is_retiring && info.HasFreshLoad(now, stale_after);
}

auto BaseAppMgr::IsBetterCandidate(const BaseAppInfo& candidate, const BaseAppInfo& incumbent)
    -> bool {
  if (candidate.effective_load != incumbent.effective_load) {
    return candidate.effective_load < incumbent.effective_load;
  }

  if (candidate.proxy_count != incumbent.proxy_count) {
    return candidate.proxy_count < incumbent.proxy_count;
  }

  if (candidate.entity_count != incumbent.entity_count) {
    return candidate.entity_count < incumbent.entity_count;
  }

  return candidate.app_id < incumbent.app_id;
}

auto BaseAppMgr::LoadReportStaleAfter() const -> Duration {
  const int kUpdateHertz = std::max(Config().update_hertz, 1);
  const auto kExpectedTick =
      std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / kUpdateHertz));
  const auto kMinimumStaleness = std::chrono::duration_cast<Duration>(std::chrono::seconds(1));
  return std::max(kExpectedTick * 10, kMinimumStaleness);
}

auto BaseAppMgr::FindLeastLoaded() const -> const BaseAppInfo* {
  const BaseAppInfo* best = nullptr;
  const auto kNow = Clock::now();
  const auto kStaleAfter = LoadReportStaleAfter();
  for (const auto& [addr, info] : baseapps_) {
    if (!IsAllocationCandidate(info, kNow, kStaleAfter)) {
      continue;
    }

    if (best == nullptr || IsBetterCandidate(info, *best)) {
      best = &info;
    }
  }
  return best;
}

auto BaseAppMgr::ShouldPreferAffinity(const BaseAppInfo& preferred,
                                      const BaseAppInfo* least_loaded) const -> bool {
  if (preferred.IsHardOverloaded(kOverloadThreshold)) {
    return false;
  }

  if (least_loaded == nullptr || preferred.app_id == least_loaded->app_id) {
    return true;
  }

  // Preserve DBID affinity using reported process load rather than the
  // queue-biased balancing score. For shortline relogin storms the preferred
  // BaseApp often carries more detached proxies precisely because it can
  // complete the reconnect locally; routing away from it defeats the fast
  // path and amplifies force-logoff pressure.
  const float kAllowedLoad = std::min(1.0f, least_loaded->measured_load + kDbidAffinityLoadSlack);
  return preferred.measured_load <= kAllowedLoad;
}

auto BaseAppMgr::FindAllocationTarget(DatabaseID dbid) -> const BaseAppInfo* {
  const auto kNow = Clock::now();
  const auto kStaleAfter = LoadReportStaleAfter();
  dbid_affinity_.PruneExpired(kNow, kDbidAffinityTtl);

  const BaseAppInfo* least_loaded = FindLeastLoaded();
  if (dbid == kInvalidDBID) {
    return least_loaded;
  }

  const auto kAffinity = dbid_affinity_.Find(dbid);
  if (!kAffinity) {
    return least_loaded;
  }

  const auto* preferred = FindBaseappByAppId(kAffinity->app_id);
  if (preferred == nullptr || !IsAllocationCandidate(*preferred, kNow, kStaleAfter)) {
    dbid_affinity_.Erase(dbid);
    return least_loaded;
  }

  if (ShouldPreferAffinity(*preferred, least_loaded)) {
    return preferred;
  }

  return least_loaded;
}

void BaseAppMgr::RecordSuccessfulAllocation(uint32_t app_id, DatabaseID dbid, TimePoint now) {
  if (auto* reserved = FindBaseappByAppId(app_id)) {
    reserved->ReserveLoginSlot(kLoginAllocationLoadIncrement);
    dbid_affinity_.Remember(dbid, reserved->app_id, now);
  }
}

auto BaseAppMgr::IsOverloaded() const -> bool {
  const auto* best = FindLeastLoaded();
  if (!best || !best->IsHardOverloaded(kOverloadThreshold)) {
    overload_start_ = {};
    logins_since_overload_ = 0;
    return false;
  }

  auto now = Clock::now();
  if (overload_start_ == TimePoint{}) {
    overload_start_ = now;
    logins_since_overload_ = 0;
  }

  auto duration = now - overload_start_;
  if (duration > std::chrono::seconds(5) || logins_since_overload_ >= kOverloadLoginLimit) {
    return true;
  }

  ++logins_since_overload_;
  return false;
}

void BaseAppMgr::BroadcastToAllBaseapps(const baseappmgr::GlobalBaseNotification& notif) {
  for (auto& [addr, info] : baseapps_) {
    if (info.is_ready && info.channel) {
      if (auto r = info.channel->SendMessage(notif); !r) {
        // A missed notification leaves peers permanently disagreeing on
        // which BaseApp owns a singleton (e.g. ChatService) - RPCs to
        // that mailbox land on stale or absent owner.  No automatic
        // resync; operator must restart or rely on death detection.
        ATLAS_LOG_ERROR(
            "BaseAppMgr: GlobalBaseNotification dropped to baseapp {}: {} "
            "— peer-view divergence on global mailbox",
            addr.ToString(), r.Error().Message());
      }
    }
  }
}

void BaseAppMgr::OnBaseappDeath(const Address& addr, uint8_t reason) {
  auto it = baseapps_.find(addr);
  if (it == baseapps_.end()) return;
  if (reason == 0) {
    ATLAS_LOG_INFO("BaseAppMgr: BaseApp app_id={} deregistered ({}:{})", it->second.app_id,
                   addr.Ip(), addr.Port());
  } else {
    ATLAS_LOG_WARNING("BaseAppMgr: BaseApp app_id={} died ({}:{})", it->second.app_id, addr.Ip(),
                      addr.Port());
  }
  dbid_affinity_.ForgetApp(it->second.app_id);
  app_id_index_.erase(it->second.app_id);
  baseapps_.erase(it);
  MarkSnapshotDirty("baseapp-death");

  // Clean up any global bases owned by the dead BaseApp
  for (auto git = global_bases_.begin(); git != global_bases_.end();) {
    if (git->second.base_addr == addr) {
      baseappmgr::GlobalBaseNotification notif;
      notif.key = git->second.key;
      notif.base_addr = addr;
      notif.entity_id = git->second.entity_id;
      notif.type_id = git->second.type_id;
      notif.added = false;
      BroadcastToAllBaseapps(notif);
      git = global_bases_.erase(git);
    } else {
      ++git;
    }
  }
}

}  // namespace atlas
