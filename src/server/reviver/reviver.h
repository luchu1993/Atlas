#ifndef ATLAS_SERVER_REVIVER_REVIVER_H_
#define ATLAS_SERVER_REVIVER_REVIVER_H_

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/clock.h"
#include "network/machined_types.h"
#include "platform/filesystem.h"
#include "platform/process_launcher.h"
#include "server/manager_app.h"

namespace atlas {

namespace cellappmgr {
struct HealthProbeAck;
}
namespace baseappmgr {
struct HealthProbeAck;
}

// Unknown / empty input falls back to "local"; caller logs the raw value.
[[nodiscard]] auto NormalizeLeaderLockMode(std::string_view configured) -> std::string;

class Reviver : public ManagerApp {
 public:
  Reviver(EventDispatcher& dispatcher, NetworkInterface& network);

  static auto Run(int argc, char* argv[]) -> int;

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void RegisterWatchers() override;
  void OnTickComplete() override;

 private:
  // One Reviver process supervises both CellAppMgr and BaseAppMgr; each
  // target keeps its own leader lock, heartbeat, restart budget, watchers.
  struct ManagedTarget {
    ProcessType process_type;
    std::string slug;  // baked into reviver/<slug>/* watcher paths
    std::string configured_name;
    // Health check + heartbeat + launch parameters, snapshotted from
    // ServerConfig at Init so a config reload doesn't shift mid-flight.
    int health_interval_ms{1000};
    int heartbeat_timeout_ms{4000};
    int manager_health_timeout_ms{5000};
    int health_failure_threshold{2};
    int audit_interval_ms{1000};
    int missing_audit_threshold{2};
    int launch_timeout_ms{5000};
    int update_hertz{10};
    bool on_start{false};

    std::filesystem::path exe;
    uint16_t internal_port{0};
    std::filesystem::path snapshot_path;
    std::filesystem::path output_path;
    int snapshot_interval_ms{-1};
    std::filesystem::path leader_lock_path;

    // "local" → leader_lock owns a ScopedFileLock; "machined" → leader_lock
    // stays nullopt and leader_lock_held + lease_* track a machined lease.
    std::string leader_lock_mode;
    std::optional<fs::ScopedFileLock> leader_lock;
    bool leader_lock_held{false};
    std::string leader_lock_holder_id;
    TimePoint leader_lock_expires_at{};
    TimePoint next_lease_renew_at{};
    bool lease_request_in_flight{false};
    uint32_t lease_failure_streak{0};
    uint64_t lease_acquire_count{0};
    uint64_t lease_renew_count{0};
    uint64_t lease_failure_count{0};
    Address last_addr;
    uint32_t last_pid{0};
    LaunchedProcess launched;
    uint64_t active_generation{0};
    bool active{false};
    TimePoint next_leader_lock_attempt{};
    TimePoint next_health_check_at{};
    TimePoint next_heartbeat_at{};
    TimePoint launch_deadline{};
    TimePoint health_check_sent_at{};
    TimePoint heartbeat_sent_at{};
    TimePoint next_registry_audit_at{};
    bool startup_checked{false};
    TimePoint startup_check_at{};
    bool query_pending{false};
    bool launch_pending{false};
    bool health_pending{false};
    bool heartbeat_pending{false};
    uint64_t health_check_generation{0};
    uint64_t heartbeat_nonce{0};
    uint64_t heartbeat_pending_nonce{0};
    TimerHandle restart_timer;
    uint32_t restart_attempts{0};
    uint32_t heartbeat_failure_streak{0};
    uint32_t manager_health_failure_streak{0};
    uint32_t registry_missing_streak{0};
    uint64_t launch_count{0};
    uint64_t launch_failures{0};
    uint64_t launch_timeout_count{0};
    bool restart_limit_reached{false};
    uint64_t restart_limit_hit_count{0};
    uint64_t liveness_failures{0};
    uint64_t health_check_count{0};
    uint64_t health_failure_count{0};
    uint64_t manager_health_failure_count{0};
    uint64_t manager_health_timeout_count{0};
    uint64_t heartbeat_sent_count{0};
    uint64_t heartbeat_ack_count{0};
    uint64_t heartbeat_failure_count{0};
    uint64_t heartbeat_timeout_count{0};
    TimePoint heartbeat_last_ack_at{};
    uint64_t heartbeat_last_game_time{0};
    uint64_t heartbeat_mgr_generation{0};
    uint64_t heartbeat_snapshot_saves{0};
    uint64_t heartbeat_snapshot_failures{0};
    bool heartbeat_snapshot_dirty{false};
    bool heartbeat_snapshot_save_stale{false};
    uint64_t forced_termination_count{0};
    uint64_t leader_lock_acquires{0};
    uint64_t leader_lock_failures{0};
    uint64_t registry_audit_count{0};
    uint64_t registry_missing_count{0};
    std::string last_error;
  };

  // Populate config-derived fields on t; t.process_type and t.slug must
  // already be set so RegisterTargetWatchers sees a non-empty slug.
  void InitTarget(ManagedTarget& t);

  // Disabled targets (no configured_name, or no exe and !on_start) are
  // skipped from all audits.
  [[nodiscard]] auto TargetEnabled(const ManagedTarget& t) const -> bool;

  // machined birth/death notification dispatch.
  void OnTargetBirth(ManagedTarget& t, const machined::BirthNotification& msg);
  void OnTargetDeath(ManagedTarget& t, const machined::DeathNotification& msg);
  void RememberTarget(ManagedTarget& t, std::string_view name, const Address& addr, uint32_t pid);

  void AuditLeadership(ManagedTarget& t);
  void AuditColdStart(ManagedTarget& t);
  void AuditTargetLaunch(ManagedTarget& t);
  void AuditTargetLiveness(ManagedTarget& t);
  void AuditTargetHeartbeat(ManagedTarget& t);
  void AuditTargetHealth(ManagedTarget& t);
  void OnTargetHealth(ManagedTarget& t, uint64_t generation, bool found);
  void AuditTargetRegistry(ManagedTarget& t);
  void OnTargetRegistryAudit(ManagedTarget& t, std::vector<machined::ProcessInfo> infos);
  void RecordTargetHeartbeatFailure(ManagedTarget& t, std::string_view reason);
  void RecordTargetManagerHealthFailure(ManagedTarget& t, std::string_view reason);
  void RecordTargetHealthFailure(ManagedTarget& t, std::string_view reason, uint32_t& streak);
  void ResetTargetHeartbeat(ManagedTarget& t);
  void ResetTargetManagerHealth(ManagedTarget& t);
  void ResetTargetLaunch(ManagedTarget& t);
  void TerminateTargetIfLocal(ManagedTarget& t, std::string_view reason);
  void TerminateLaunchedTarget(ManagedTarget& t, std::string_view reason);
  void ScheduleTargetRestart(ManagedTarget& t, Duration delay);
  void LaunchTarget(ManagedTarget& t);
  void SendHeartbeat(ManagedTarget& t);

  void OnCellAppMgrHeartbeatAck(const Address& src, Channel* ch,
                                const cellappmgr::HealthProbeAck& msg);
  void OnBaseAppMgrHeartbeatAck(const Address& src, Channel* ch,
                                const baseappmgr::HealthProbeAck& msg);
  void RecordHeartbeatAck(ManagedTarget& t, const Address& src, uint64_t nonce, uint64_t game_time,
                          uint64_t snapshot_saves, uint64_t snapshot_failures,
                          uint64_t mgr_generation, bool snapshot_dirty, bool snapshot_save_stale);

  [[nodiscard]] auto HasLeadership(const ManagedTarget& t) const -> bool;
  [[nodiscard]] auto MatchesTargetName(const ManagedTarget& t, std::string_view name) const -> bool;
  [[nodiscard]] auto CanCheckLocalPid() const -> bool;
  [[nodiscard]] auto ResolveLeaderLockPath(const ManagedTarget& t) const -> std::filesystem::path;
  [[nodiscard]] auto LeaderLockContent() const -> std::string;
  [[nodiscard]] auto TargetStatus(const ManagedTarget& t) const -> std::string;
  [[nodiscard]] auto HeartbeatLastAckAgeMsForWatcher(const ManagedTarget& t) const -> int64_t;
  [[nodiscard]] auto TargetHeartbeatSnapshotStatus(const ManagedTarget& t) const -> std::string;

  void RegisterTargetWatchers(ManagedTarget& t);

  std::filesystem::path self_exe_;
  ManagedTarget cellappmgr_target_;
  ManagedTarget baseappmgr_target_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_REVIVER_REVIVER_H_
