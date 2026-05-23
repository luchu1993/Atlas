#ifndef ATLAS_SERVER_REVIVER_REVIVER_H_
#define ATLAS_SERVER_REVIVER_REVIVER_H_

#include <cstdint>
#include <filesystem>
#include <string>

#include "foundation/clock.h"
#include "network/machined_types.h"
#include "platform/process_launcher.h"
#include "server/manager_app.h"

namespace atlas {

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
  void AuditColdStart();
  void ScheduleCellAppMgrRestart(Duration delay);
  void LaunchCellAppMgr();

  [[nodiscard]] auto MatchesTargetName(std::string_view name) const -> bool;
  [[nodiscard]] auto ResolveCellAppMgrExe() const -> std::filesystem::path;
  [[nodiscard]] auto CellAppMgrPortForLaunch() const -> uint16_t;

  std::filesystem::path self_exe_;
  Address last_cellappmgr_addr_;
  uint32_t last_cellappmgr_pid_{0};
  uint32_t launched_cellappmgr_pid_{0};
  bool cellappmgr_active_{false};
  bool startup_checked_{false};
  TimePoint startup_check_at_{};
  TimerHandle restart_timer_;
  uint32_t restart_attempts_{0};
  uint64_t launch_count_{0};
  uint64_t launch_failures_{0};
  std::string last_error_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_REVIVER_REVIVER_H_
