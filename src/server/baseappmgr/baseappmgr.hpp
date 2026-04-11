#pragma once

#include "baseappmgr_messages.hpp"
#include "foundation/time.hpp"
#include "loginapp/login_messages.hpp"
#include "server/entity_types.hpp"
#include "server/manager_app.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>

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
        float load{0.0f};
        uint32_t entity_count{0};
        uint32_t proxy_count{0};
        bool is_ready{false};
        bool is_retiring{false};
        Channel* channel{nullptr};
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
    [[nodiscard]] auto find_least_loaded() const -> const BaseAppInfo*;
    [[nodiscard]] auto is_overloaded() const -> bool;
    void broadcast_to_all_baseapps(const baseappmgr::GlobalBaseNotification& notif);
    auto allocate_entity_id_range() -> std::pair<EntityID, EntityID>;
    void on_baseapp_death(const Address& addr);

    // ---- State ----------------------------------------------------------
    std::unordered_map<Address, BaseAppInfo> baseapps_;
    uint32_t next_app_id_{1};
    EntityID next_entity_range_start_{1};

    static constexpr uint32_t kEntityIdRangeSize = 10'000u;
    static constexpr float kOverloadThreshold = 0.9f;
    static constexpr int kOverloadLoginLimit = 5;

    // Overload state
    mutable TimePoint overload_start_{};
    mutable int logins_since_overload_{0};

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
