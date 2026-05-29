#ifndef ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_H_
#define ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_H_

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "baseappmgr_messages.h"
#include "foundation/clock.h"
#include "foundation/error.h"
#include "loginapp/login_messages.h"
#include "server/entity_types.h"
#include "server/manager_app.h"

namespace atlas {

class Channel;
namespace machined {
struct ProcessInfo;
}

class BaseAppMgr : public ManagerApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  BaseAppMgr(EventDispatcher& dispatcher, NetworkInterface& network);

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void RegisterWatchers() override;
  void OnTickComplete() override;

 public:
  // Serialise the authoritative BaseAppMgr state (BaseApp table,
  // next_app_id_, dbid_affinity) into a checksummed envelope. Used by
  // SaveSnapshotToFile and by Reviver-driven recovery.
  [[nodiscard]] auto Snapshot() const -> std::vector<std::byte>;

  // Replace this instance's authoritative state from a snapshot blob.
  // Restored BaseApps come back in needs_reattach state until the real
  // process reconnects via OnRegisterBaseapp.
  [[nodiscard]] auto Restore(std::span<const std::byte> bytes) -> Result<void>;

  [[nodiscard]] auto SaveSnapshotToFile(const std::filesystem::path& path) -> Result<void>;
  [[nodiscard]] auto RestoreSnapshotFromFile(const std::filesystem::path& path) -> Result<void>;

  void RegisterWatchersForTest() { RegisterWatchers(); }
  void ApplyReattachRegistryAuditForTest(std::span<const machined::ProcessInfo> infos);
  void OnReattachRegistryAuditForTest(std::vector<machined::ProcessInfo> infos);
  // Seeds a reattach-pending BaseApp as Restore would, without needing a live
  // Channel (OnRegisterBaseapp derefs ch on the fresh-register path).
  void SeedRestoredBaseAppForTest(const Address& internal_addr, uint32_t app_id);

 private:
  struct BaseAppInfo {
    Address internal_addr;
    Address external_addr;
    uint32_t app_id{0};
    float measured_load{0.0f};
    float effective_load{0.0f};
    uint32_t entity_count{0};
    uint32_t proxy_count{0};
    uint32_t pending_prepare_count{0};
    uint32_t pending_force_logoff_count{0};
    uint32_t detached_proxy_count{0};
    uint32_t logoff_in_flight_count{0};
    uint32_t deferred_login_count{0};
    uint32_t pending_login_allocations{0};
    bool is_ready{false};
    bool is_retiring{false};
    bool needs_reattach{false};
    bool restored_from_snapshot{false};
    Channel* channel{nullptr};
    TimePoint last_load_report_at{};
    TimePoint registered_at{};
    TimePoint last_reattach_watchdog_log_at{};

    void ApplyLoadReport(float load, uint32_t reported_entity_count, uint32_t reported_proxy_count,
                         uint32_t reported_pending_prepare_count,
                         uint32_t reported_pending_force_logoff_count,
                         uint32_t reported_detached_proxy_count,
                         uint32_t reported_logoff_in_flight_count,
                         uint32_t reported_deferred_login_count, TimePoint now);
    void ReserveLoginSlot(float load_increment);
    [[nodiscard]] auto HasFreshLoad(TimePoint now, Duration stale_after) const -> bool;
    [[nodiscard]] auto QueuePressure() const -> float;
    [[nodiscard]] auto IsHardOverloaded(float overload_threshold) const -> bool;
  };

  struct DbidAffinityTable {
    struct Entry {
      uint32_t app_id{0};
      TimePoint last_assigned_at{};
    };

    void Remember(DatabaseID dbid, uint32_t app_id, TimePoint now);
    void Erase(DatabaseID dbid);
    void ForgetApp(uint32_t app_id);
    void PruneExpired(TimePoint now, Duration ttl);
    [[nodiscard]] auto Find(DatabaseID dbid) const -> std::optional<Entry>;
    [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }
    [[nodiscard]] auto Entries() const -> const std::unordered_map<DatabaseID, Entry>& {
      return entries_;
    }
    void Clear() {
      entries_.clear();
      dbids_by_app_.clear();
    }

   private:
    std::unordered_map<DatabaseID, Entry> entries_;
    std::unordered_map<uint32_t, std::unordered_set<DatabaseID>> dbids_by_app_;
  };

  void OnRegisterBaseapp(const Address& src, Channel* ch, const baseappmgr::RegisterBaseApp& msg);
  void OnBaseappReady(const Address& src, Channel* ch, const baseappmgr::BaseAppReady& msg);
  void OnInformLoad(const Address& src, Channel* ch, const baseappmgr::InformLoad& msg);
  void OnHealthProbe(const Address& src, Channel* ch, const baseappmgr::HealthProbe& msg);
  void OnAllocateBaseapp(const Address& src, Channel* ch, const login::AllocateBaseApp& msg);
  [[nodiscard]] auto FindBaseappByAppId(uint32_t app_id) -> BaseAppInfo*;
  [[nodiscard]] auto FindBaseappByAppId(uint32_t app_id) const -> const BaseAppInfo*;
  [[nodiscard]] auto MatchesRegisteredSource(const BaseAppInfo& info, const Address& src,
                                             const Channel* ch, std::string_view operation) const
      -> bool;
  [[nodiscard]] auto IsAllocationCandidate(const BaseAppInfo& info, TimePoint now,
                                           Duration stale_after) const -> bool;
  [[nodiscard]] static auto IsBetterCandidate(const BaseAppInfo& candidate,
                                              const BaseAppInfo& incumbent) -> bool;
  [[nodiscard]] auto ShouldPreferAffinity(const BaseAppInfo& preferred,
                                          const BaseAppInfo* least_loaded) const -> bool;
  [[nodiscard]] auto LoadReportStaleAfter() const -> Duration;
  [[nodiscard]] auto FindLeastLoaded() const -> const BaseAppInfo*;
  [[nodiscard]] auto FindAllocationTarget(DatabaseID dbid) -> const BaseAppInfo*;
  void RecordSuccessfulAllocation(uint32_t app_id, DatabaseID dbid, TimePoint now);
  [[nodiscard]] auto IsOverloaded() const -> bool;
  void OnBaseappDeath(const Address& addr, uint8_t reason);

  std::unordered_map<Address, BaseAppInfo> baseapps_;
  std::unordered_map<uint32_t, Address> app_id_index_;
  uint32_t next_app_id_{1};
  static constexpr float kLoginAllocationLoadIncrement = 0.01f;
  static constexpr float kOverloadThreshold = 0.9f;
  static constexpr float kDbidAffinityLoadSlack = 0.25f;
  static constexpr int kOverloadLoginLimit = 5;
  static constexpr uint32_t kHardOverloadPendingPrepareLimit = 1024u;
  static constexpr uint32_t kHardOverloadDeferredLoginLimit = 1024u;
  static constexpr uint32_t kHardOverloadLogoffLimit = 1024u;
  static constexpr auto kDbidAffinityTtl = std::chrono::seconds(30);

  mutable TimePoint overload_start_{};
  mutable int logins_since_overload_{0};
  DbidAffinityTable dbid_affinity_;

  // Snapshot machinery (mirrors CellAppMgr; share via snapshot_envelope.h
  // in a follow-up refactor).
  void MarkSnapshotDirty(const char* reason);
  void SaveConfiguredSnapshot(const char* context);
  void AuditReattachWatchdog();
  // Reconcile reattach-pending BaseApps against machined truth: a host that
  // died during mgr downtime never re-registers, so the watchdog would log it
  // forever and its dbid_affinity entries would dangle. Prune the ones
  // machined reports gone (mirrors CellAppMgr, minus the leaf-rehome path —
  // BaseApps own no topology, so pruning is always safe).
  void AuditReattachRegistry();
  void OnReattachRegistryAudit(std::vector<machined::ProcessInfo> infos);
  [[nodiscard]] auto ApplyReattachRegistryAudit(std::span<const machined::ProcessInfo> infos)
      -> std::size_t;
  [[nodiscard]] auto BuildReattachRegistryStatusSummary() const -> std::string;
  [[nodiscard]] auto ReattachWatchdogWindow() const -> Duration;
  [[nodiscard]] auto IsReattachStuck(const BaseAppInfo& info, TimePoint now) const -> bool;
  [[nodiscard]] auto RestoredBaseAppCount() const -> std::size_t;
  [[nodiscard]] auto PendingReattachBaseAppCount() const -> std::size_t;
  [[nodiscard]] auto CompletedReattachBaseAppCount() const -> std::size_t;
  [[nodiscard]] auto StuckReattachBaseAppCount() const -> std::size_t;
  [[nodiscard]] auto ReattachCompleted() const -> bool;
  [[nodiscard]] auto ReattachStateForWatcher() const -> std::string;
  [[nodiscard]] auto BuildReattachStatusSummary() const -> std::string;
  [[nodiscard]] auto SnapshotFilePathForWatcher() const -> std::string;
  [[nodiscard]] auto SnapshotFilePresentForWatcher() const -> bool;
  [[nodiscard]] auto SnapshotFileBytesForWatcher() const -> uint64_t;
  [[nodiscard]] auto BuildSnapshotFileStatusSummary() const -> std::string;
  [[nodiscard]] auto SnapshotBackupPathForWatcher() const -> std::string;
  [[nodiscard]] auto SnapshotBackupPresentForWatcher() const -> bool;
  [[nodiscard]] auto SnapshotBackupBytesForWatcher() const -> uint64_t;
  [[nodiscard]] auto BuildSnapshotBackupStatusSummary() const -> std::string;
  [[nodiscard]] auto LastSnapshotAttemptAgeMsForWatcher() const -> int64_t;
  [[nodiscard]] auto LastSnapshotSaveAgeMsForWatcher() const -> int64_t;
  [[nodiscard]] auto LastSnapshotDirtyAgeMsForWatcher() const -> int64_t;
  [[nodiscard]] auto LastSnapshotRestoreAttemptAgeMsForWatcher() const -> int64_t;
  [[nodiscard]] auto LastSnapshotRestoreAgeMsForWatcher() const -> int64_t;
  [[nodiscard]] auto SnapshotSaveStaleForWatcher() const -> bool;
  [[nodiscard]] auto SnapshotSizeHighWaterPct() const -> uint32_t;
  [[nodiscard]] auto BuildSnapshotStatusSummary() const -> std::string;
  [[nodiscard]] auto BuildSnapshotRestoreStatusSummary() const -> std::string;

  TimePoint last_snapshot_attempt_at_{};
  TimePoint last_snapshot_save_at_{};
  TimePoint last_snapshot_restore_attempt_at_{};
  TimePoint last_snapshot_restore_at_{};
  uint64_t snapshot_save_count_{0};
  uint64_t snapshot_restore_count_{0};
  uint64_t snapshot_fallback_restore_count_{0};
  uint64_t snapshot_save_failure_count_{0};
  uint64_t snapshot_restore_failure_count_{0};
  uint64_t snapshot_failure_count_{0};
  uint64_t snapshot_backup_skip_count_{0};
  std::size_t last_snapshot_bytes_{0};
  std::filesystem::path last_snapshot_save_path_;
  std::string last_snapshot_save_error_;
  TimePoint last_snapshot_save_warning_at_{};
  TimePoint last_snapshot_size_warning_at_{};
  bool snapshot_dirty_{false};
  TimePoint snapshot_dirty_at_{};
  std::string snapshot_dirty_reason_;
  std::string last_snapshot_restore_source_{"none"};
  std::filesystem::path last_snapshot_restore_path_;
  std::string last_snapshot_restore_error_;
  std::string last_snapshot_restore_primary_error_;

  TimePoint last_reattach_registry_audit_at_{};
  bool reattach_registry_audit_pending_{false};
  uint64_t reattach_registry_audit_count_{0};
  uint64_t reattach_registry_reconciled_total_{0};
  std::size_t last_reattach_registry_missing_{0};
  std::string last_reattach_registry_error_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_H_
