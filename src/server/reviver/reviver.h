#ifndef ATLAS_SERVER_REVIVER_REVIVER_H_
#define ATLAS_SERVER_REVIVER_REVIVER_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
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
  void OnCellAppMgrBirth(const machined::BirthNotification& msg);
  void OnCellAppMgrDeath(const machined::DeathNotification& msg);
  void RememberCellAppMgr(std::string_view name, const Address& addr, uint32_t pid);
  void AuditLeadership();
  void AuditColdStart();
  void AuditCellAppMgrLaunch();
  void AuditCellAppMgrLiveness();
  void AuditCellAppMgrHeartbeat();
  void AuditCellAppMgrHealth();
  void OnCellAppMgrHeartbeatAck(const Address& src, Channel* ch,
                                const cellappmgr::HealthProbeAck& msg);
  void OnCellAppMgrHealth(uint64_t generation, bool found);
  void AuditCellAppMgrRegistry();
  void OnCellAppMgrRegistryAudit(std::vector<machined::ProcessInfo> infos);
  void RecordCellAppMgrHeartbeatFailure(std::string_view reason);
  void RecordCellAppMgrManagerHealthFailure(std::string_view reason);
  void RecordCellAppMgrHealthFailure(std::string_view reason, uint32_t& streak);
  void ResetCellAppMgrHeartbeat();
  void ResetCellAppMgrManagerHealth();
  void ResetCellAppMgrLaunch();
  void TerminateCellAppMgrIfLocal(std::string_view reason);
  void TerminateLaunchedCellAppMgr(std::string_view reason);
  void ScheduleCellAppMgrRestart(Duration delay);
  void LaunchCellAppMgr();

  [[nodiscard]] auto HasLeadership() const -> bool;
  [[nodiscard]] auto MatchesTargetName(std::string_view name) const -> bool;
  [[nodiscard]] auto CanCheckLocalPid() const -> bool;
  [[nodiscard]] auto ResolveLeaderLockPath() const -> std::filesystem::path;
  [[nodiscard]] auto ResolveCellAppMgrExe() const -> std::filesystem::path;
  [[nodiscard]] auto CellAppMgrPortForLaunch() const -> uint16_t;
  [[nodiscard]] auto LeaderLockContent() const -> std::string;
  [[nodiscard]] auto CellAppMgrStatus() const -> std::string;
  [[nodiscard]] auto HeartbeatLastAckAgeMsForWatcher() const -> int64_t;
  [[nodiscard]] auto CellAppMgrHeartbeatSnapshotStatus() const -> std::string;

  std::filesystem::path self_exe_;
  std::filesystem::path leader_lock_path_;
  std::optional<fs::ScopedFileLock> leader_lock_;
  Address last_cellappmgr_addr_;
  uint32_t last_cellappmgr_pid_{0};
  uint32_t launched_cellappmgr_pid_{0};
  uint64_t cellappmgr_active_generation_{0};
  bool cellappmgr_active_{false};
  TimePoint next_leader_lock_attempt_{};
  TimePoint next_health_check_at_{};
  TimePoint next_heartbeat_at_{};
  TimePoint launch_deadline_{};
  TimePoint health_check_sent_at_{};
  TimePoint heartbeat_sent_at_{};
  TimePoint next_registry_audit_at_{};
  bool startup_checked_{false};
  TimePoint startup_check_at_{};
  bool cellappmgr_query_pending_{false};
  bool launch_pending_{false};
  bool cellappmgr_health_pending_{false};
  bool heartbeat_pending_{false};
  uint64_t health_check_generation_{0};
  uint64_t heartbeat_nonce_{0};
  uint64_t heartbeat_pending_nonce_{0};
  TimerHandle restart_timer_;
  uint32_t restart_attempts_{0};
  uint32_t heartbeat_failure_streak_{0};
  uint32_t manager_health_failure_streak_{0};
  uint32_t registry_missing_streak_{0};
  uint64_t launch_count_{0};
  uint64_t launch_failures_{0};
  uint64_t launch_timeout_count_{0};
  bool restart_limit_reached_{false};
  uint64_t restart_limit_hit_count_{0};
  uint64_t liveness_failures_{0};
  uint64_t health_check_count_{0};
  uint64_t health_failure_count_{0};
  uint64_t manager_health_failure_count_{0};
  uint64_t manager_health_timeout_count_{0};
  uint64_t heartbeat_sent_count_{0};
  uint64_t heartbeat_ack_count_{0};
  uint64_t heartbeat_failure_count_{0};
  uint64_t heartbeat_timeout_count_{0};
  TimePoint heartbeat_last_ack_at_{};
  uint64_t heartbeat_last_game_time_{0};
  uint64_t heartbeat_snapshot_saves_{0};
  uint64_t heartbeat_snapshot_failures_{0};
  bool heartbeat_snapshot_dirty_{false};
  bool heartbeat_snapshot_save_stale_{false};
  uint64_t forced_termination_count_{0};
  uint64_t leader_lock_acquires_{0};
  uint64_t leader_lock_failures_{0};
  uint64_t registry_audit_count_{0};
  uint64_t registry_missing_count_{0};
  std::string last_error_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_REVIVER_REVIVER_H_
