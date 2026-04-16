#ifndef ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_H_
#define ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "baseappmgr_messages.h"
#include "foundation/clock.h"
#include "loginapp/login_messages.h"
#include "server/entity_types.h"
#include "server/manager_app.h"

namespace atlas {

class Channel;

// ============================================================================
// BaseAppMgr — BaseApp cluster manager
//
// Responsibilities:
//   • Accept BaseApp registration and assign app_id + EntityID range
//   • Track BaseApp load (InformLoad) for least-loaded selection
//   • Allocate BaseApps for new logins (AllocateBaseApp)
//   • Manage Global Bases registry and broadcast to all BaseApps
//   • Handle BaseApp death (machined events)
// ============================================================================

class BaseAppMgr : public ManagerApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  BaseAppMgr(EventDispatcher& dispatcher, NetworkInterface& network);

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;
  void RegisterWatchers() override;

 private:
  // ---- BaseApp tracking -----------------------------------------------
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
    Channel* channel{nullptr};
    TimePoint last_load_report_at{};

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

   private:
    std::unordered_map<DatabaseID, Entry> entries_;
    std::unordered_map<uint32_t, std::unordered_set<DatabaseID>> dbids_by_app_;
  };

  // ---- Message handlers -----------------------------------------------
  void OnRegisterBaseapp(const Address& src, Channel* ch, const baseappmgr::RegisterBaseApp& msg);
  void OnBaseappReady(const Address& src, Channel* ch, const baseappmgr::BaseAppReady& msg);
  void OnInformLoad(const Address& src, Channel* ch, const baseappmgr::InformLoad& msg);
  void OnAllocateBaseapp(const Address& src, Channel* ch, const login::AllocateBaseApp& msg);
  void OnRegisterGlobalBase(const Address& src, Channel* ch,
                            const baseappmgr::RegisterGlobalBase& msg);
  void OnDeregisterGlobalBase(const Address& src, Channel* ch,
                              const baseappmgr::DeregisterGlobalBase& msg);
  // ---- Internal helpers -----------------------------------------------
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
  void BroadcastToAllBaseapps(const baseappmgr::GlobalBaseNotification& notif);
  void OnBaseappDeath(const Address& addr);

  // ---- State ----------------------------------------------------------
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

  // Overload state
  mutable TimePoint overload_start_{};
  mutable int logins_since_overload_{0};
  DbidAffinityTable dbid_affinity_;

  // Global Bases
  struct GlobalBaseEntry {
    std::string key;
    Address base_addr;
    EntityID entity_id{kInvalidEntityID};
    uint16_t type_id{0};
  };
  std::unordered_map<std::string, GlobalBaseEntry> global_bases_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPPMGR_BASEAPPMGR_H_
