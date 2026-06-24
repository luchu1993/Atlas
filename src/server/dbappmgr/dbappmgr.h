#ifndef ATLAS_SERVER_DBAPPMGR_DBAPPMGR_H_
#define ATLAS_SERVER_DBAPPMGR_DBAPPMGR_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "dbappmgr_messages.h"
#include "foundation/clock.h"
#include "server/manager_app.h"

namespace atlas {

class Channel;

class DBAppMgr : public ManagerApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  DBAppMgr(EventDispatcher& dispatcher, NetworkInterface& network);

  void OnRegisterDbApp(const Address& src, Channel* ch, const dbappmgr::RegisterDbApp& msg);
  void OnInformLoad(const Address& src, Channel* ch, const dbappmgr::InformLoad& msg);
  void OnGetShardTable(const Address& src, Channel* ch, const dbappmgr::GetShardTable& msg);
  void OnRecoverDBAppState(const Address& src, Channel* ch, const dbappmgr::RecoverDBAppState& msg);
  void OnHealthProbe(const Address& src, Channel* ch, const dbappmgr::HealthProbe& msg);
  void OnDbAppDeath(const Address& internal_addr, uint8_t reason);

  void RegisterWatchersForTest() { RegisterWatchers(); }
  void SetRecoveryDeadlineForTest(TimePoint t) { recovery_deadline_ = t; }

  struct DBAppInfo {
    Address internal_addr;
    uint32_t app_id{0};
    float load{0.0f};
    uint32_t entity_count{0};
    uint32_t pending_checkout_count{0};
    uint32_t write_queue_depth{0};
    bool is_retiring{false};
    Channel* channel{nullptr};
    TimePoint registered_at{};
    TimePoint last_load_report_at{};
  };

  [[nodiscard]] auto DbApps() const -> const std::unordered_map<Address, DBAppInfo>& {
    return dbapps_;
  }
  [[nodiscard]] auto ShardTable() const -> const std::vector<dbappmgr::ShardEntry>& {
    return shard_table_;
  }
  [[nodiscard]] auto ShardTableVersion() const -> uint32_t { return shard_table_version_; }
  [[nodiscard]] auto FindShard(DatabaseID dbid) const -> std::optional<dbappmgr::ShardEntry>;
  [[nodiscard]] auto ShardTableSummary() const -> std::string;

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void RegisterWatchers() override;

 private:
  [[nodiscard]] auto FindDbAppByAppId(uint32_t app_id) -> DBAppInfo*;
  [[nodiscard]] auto FindDbAppByAppId(uint32_t app_id) const -> const DBAppInfo*;
  [[nodiscard]] auto MatchesRegisteredSource(const DBAppInfo& info, const Address& src,
                                             const Channel* ch, std::string_view operation) const
      -> bool;
  [[nodiscard]] auto FindLeastLoaded() const -> const DBAppInfo*;
  [[nodiscard]] static auto IsBetterCandidate(const DBAppInfo& candidate,
                                              const DBAppInfo& incumbent) -> bool;
  [[nodiscard]] static auto ResolveAdvertisedAddr(const Address& advertised, const Address& src)
      -> Address;

  auto AllocateAppId(uint32_t known_app_id) -> uint32_t;
  void AssignInitialShard(uint32_t app_id, const Address& addr);
  void SplitLargestShardFor(uint32_t app_id, const Address& addr);
  [[nodiscard]] auto RecoveryWindowActive() const -> bool;
  [[nodiscard]] auto ShouldDeferShardAssignment(const dbappmgr::RegisterDbApp& msg) const -> bool;
  void ReassignShards(uint32_t dead_app_id);
  void NormalizeShardTable();
  void BumpShardTableVersion();
  void BroadcastShardTableUpdate();
  [[nodiscard]] auto BuildShardTableResponse(uint32_t request_id, uint32_t known_version) const
      -> dbappmgr::ShardTableResponse;

  std::unordered_map<Address, DBAppInfo> dbapps_;
  std::unordered_map<uint32_t, Address> app_id_index_;
  std::unordered_map<Address, Channel*> shard_table_subscribers_;
  std::vector<dbappmgr::ShardEntry> shard_table_;
  uint32_t next_app_id_{1};
  uint32_t shard_table_version_{0};
  static constexpr Duration kStartupRecoveryWindowDefault =
      std::chrono::duration_cast<Duration>(std::chrono::seconds(2));
  Duration startup_recovery_window_{kStartupRecoveryWindowDefault};
  TimePoint recovery_deadline_{};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_DBAPPMGR_DBAPPMGR_H_
