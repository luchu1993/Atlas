#pragma once

#include "baseappmgr_messages.hpp"
#include "foundation/time.hpp"
#include "loginapp/login_messages.hpp"
#include "server/entity_types.hpp"
#include "server/manager_app.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace atlas
{

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

class BaseAppMgr : public ManagerApp
{
public:
    static auto run(int argc, char* argv[]) -> int;

    BaseAppMgr(EventDispatcher& dispatcher, NetworkInterface& network);

protected:
    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;
    void register_watchers() override;

private:
    // ---- BaseApp tracking -----------------------------------------------
    struct BaseAppInfo
    {
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

        void apply_load_report(float load, uint32_t reported_entity_count,
                               uint32_t reported_proxy_count,
                               uint32_t reported_pending_prepare_count,
                               uint32_t reported_pending_force_logoff_count,
                               uint32_t reported_detached_proxy_count,
                               uint32_t reported_logoff_in_flight_count,
                               uint32_t reported_deferred_login_count, TimePoint now);
        void reserve_login_slot(float load_increment);
        [[nodiscard]] auto has_fresh_load(TimePoint now, Duration stale_after) const -> bool;
        [[nodiscard]] auto queue_pressure() const -> float;
        [[nodiscard]] auto is_hard_overloaded(float overload_threshold) const -> bool;
    };

    struct DbidAffinityTable
    {
        struct Entry
        {
            uint32_t app_id{0};
            TimePoint last_assigned_at{};
        };

        void remember(DatabaseID dbid, uint32_t app_id, TimePoint now);
        void erase(DatabaseID dbid);
        void forget_app(uint32_t app_id);
        void prune_expired(TimePoint now, Duration ttl);
        [[nodiscard]] auto find(DatabaseID dbid) const -> std::optional<Entry>;
        [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }

    private:
        std::unordered_map<DatabaseID, Entry> entries_;
        std::unordered_map<uint32_t, std::unordered_set<DatabaseID>> dbids_by_app_;
    };

    // ---- Message handlers -----------------------------------------------
    void on_register_baseapp(const Address& src, Channel* ch,
                             const baseappmgr::RegisterBaseApp& msg);
    void on_baseapp_ready(const Address& src, Channel* ch, const baseappmgr::BaseAppReady& msg);
    void on_inform_load(const Address& src, Channel* ch, const baseappmgr::InformLoad& msg);
    void on_allocate_baseapp(const Address& src, Channel* ch, const login::AllocateBaseApp& msg);
    void on_register_global_base(const Address& src, Channel* ch,
                                 const baseappmgr::RegisterGlobalBase& msg);
    void on_deregister_global_base(const Address& src, Channel* ch,
                                   const baseappmgr::DeregisterGlobalBase& msg);
    void on_request_entity_id_range(const Address& src, Channel* ch,
                                    const baseappmgr::RequestEntityIdRange& msg);

    // ---- Internal helpers -----------------------------------------------
    [[nodiscard]] auto find_baseapp_by_app_id(uint32_t app_id) -> BaseAppInfo*;
    [[nodiscard]] auto find_baseapp_by_app_id(uint32_t app_id) const -> const BaseAppInfo*;
    [[nodiscard]] auto matches_registered_source(const BaseAppInfo& info, const Address& src,
                                                 const Channel* ch,
                                                 std::string_view operation) const -> bool;
    [[nodiscard]] auto is_allocation_candidate(const BaseAppInfo& info, TimePoint now,
                                               Duration stale_after) const -> bool;
    [[nodiscard]] static auto is_better_candidate(const BaseAppInfo& candidate,
                                                  const BaseAppInfo& incumbent) -> bool;
    [[nodiscard]] auto should_prefer_affinity(const BaseAppInfo& preferred,
                                              const BaseAppInfo* least_loaded) const -> bool;
    [[nodiscard]] auto load_report_stale_after() const -> Duration;
    [[nodiscard]] auto find_least_loaded() const -> const BaseAppInfo*;
    [[nodiscard]] auto find_allocation_target(DatabaseID dbid) -> const BaseAppInfo*;
    void record_successful_allocation(uint32_t app_id, DatabaseID dbid, TimePoint now);
    [[nodiscard]] auto is_overloaded() const -> bool;
    void broadcast_to_all_baseapps(const baseappmgr::GlobalBaseNotification& notif);
    auto allocate_entity_id_range() -> std::pair<EntityID, EntityID>;
    void on_baseapp_death(const Address& addr);

    // ---- State ----------------------------------------------------------
    std::unordered_map<Address, BaseAppInfo> baseapps_;
    std::unordered_map<uint32_t, Address> app_id_index_;
    uint32_t next_app_id_{1};
    EntityID next_entity_range_start_{1};

    static constexpr uint32_t kEntityIdRangeSize = 10'000u;
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
    struct GlobalBaseEntry
    {
        std::string key;
        Address base_addr;
        EntityID entity_id{kInvalidEntityID};
        uint16_t type_id{0};
    };
    std::unordered_map<std::string, GlobalBaseEntry> global_bases_;
};

}  // namespace atlas
