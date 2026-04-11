#include "baseappmgr.hpp"

#include "foundation/log.hpp"
#include "foundation/time.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "server/watcher.hpp"

#include <format>

namespace atlas
{

// ============================================================================
// run — static entry point
// ============================================================================

auto BaseAppMgr::run(int argc, char* argv[]) -> int
{
    EventDispatcher dispatcher;
    NetworkInterface network(dispatcher);
    BaseAppMgr app(dispatcher, network);
    return app.run_app(argc, argv);
}

BaseAppMgr::BaseAppMgr(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network)
{
}

// ============================================================================
// init
// ============================================================================

auto BaseAppMgr::init(int argc, char* argv[]) -> bool
{
    if (!ManagerApp::init(argc, argv))
        return false;

    auto& table = network().interface_table();

    (void)table.register_typed_handler<baseappmgr::RegisterBaseApp>(
        [this](const Address& src, Channel* ch, const baseappmgr::RegisterBaseApp& msg)
        { on_register_baseapp(src, ch, msg); });

    (void)table.register_typed_handler<baseappmgr::BaseAppReady>(
        [this](const Address& src, Channel* ch, const baseappmgr::BaseAppReady& msg)
        { on_baseapp_ready(src, ch, msg); });

    (void)table.register_typed_handler<baseappmgr::InformLoad>(
        [this](const Address& src, Channel* ch, const baseappmgr::InformLoad& msg)
        { on_inform_load(src, ch, msg); });

    (void)table.register_typed_handler<login::AllocateBaseApp>(
        [this](const Address& src, Channel* ch, const login::AllocateBaseApp& msg)
        { on_allocate_baseapp(src, ch, msg); });

    (void)table.register_typed_handler<baseappmgr::RegisterGlobalBase>(
        [this](const Address& src, Channel* ch, const baseappmgr::RegisterGlobalBase& msg)
        { on_register_global_base(src, ch, msg); });

    (void)table.register_typed_handler<baseappmgr::DeregisterGlobalBase>(
        [this](const Address& src, Channel* ch, const baseappmgr::DeregisterGlobalBase& msg)
        { on_deregister_global_base(src, ch, msg); });

    (void)table.register_typed_handler<baseappmgr::RequestEntityIdRange>(
        [this](const Address& src, Channel* ch, const baseappmgr::RequestEntityIdRange& msg)
        { on_request_entity_id_range(src, ch, msg); });

    // Subscribe to BaseApp death notifications
    machined_client().subscribe(machined::ListenerType::Death, ProcessType::BaseApp, nullptr,
                                [this](const machined::DeathNotification& n)
                                { on_baseapp_death(n.internal_addr); });

    ATLAS_LOG_INFO("BaseAppMgr: initialised");
    return true;
}

void BaseAppMgr::fini()
{
    ManagerApp::fini();
}

// ============================================================================
// register_watchers
// ============================================================================

void BaseAppMgr::register_watchers()
{
    ManagerApp::register_watchers();
    auto& wr = watcher_registry();
    wr.add<std::size_t>("baseappmgr/baseapp_count",
                        std::function<std::size_t()>([this] { return baseapps_.size(); }));
    wr.add<std::size_t>("baseappmgr/global_base_count",
                        std::function<std::size_t()>([this] { return global_bases_.size(); }));
}

// ============================================================================
// Message handlers
// ============================================================================

void BaseAppMgr::on_register_baseapp(const Address& /*src*/, Channel* ch,
                                     const baseappmgr::RegisterBaseApp& msg)
{
    auto [range_start, range_end] = allocate_entity_id_range();

    uint32_t app_id = next_app_id_++;
    BaseAppInfo info;
    info.internal_addr = msg.internal_addr;
    info.external_addr = msg.external_addr;
    info.app_id = app_id;
    info.channel = ch;
    baseapps_.emplace(msg.internal_addr, std::move(info));

    baseappmgr::RegisterBaseAppAck ack;
    ack.success = true;
    ack.app_id = app_id;
    ack.entity_id_start = range_start;
    ack.entity_id_end = range_end;
    ack.game_time = game_time();
    (void)ch->send_message(ack);

    ATLAS_LOG_INFO(
        "BaseAppMgr: BaseApp registered app_id={} internal={}:{} external={}:{} "
        "id_range=[{},{}]",
        app_id, msg.internal_addr.ip(), msg.internal_addr.port(), msg.external_addr.ip(),
        msg.external_addr.port(), range_start, range_end);
}

void BaseAppMgr::on_baseapp_ready(const Address& src, Channel* /*ch*/,
                                  const baseappmgr::BaseAppReady& msg)
{
    auto it = baseapps_.find(src);
    if (it == baseapps_.end())
    {
        ATLAS_LOG_WARNING("BaseAppMgr: BaseAppReady from unknown addr {}:{}", src.ip(), src.port());
        return;
    }
    it->second.is_ready = true;
    ATLAS_LOG_INFO("BaseAppMgr: BaseApp app_id={} is ready", msg.app_id);
}

void BaseAppMgr::on_inform_load(const Address& src, Channel* /*ch*/,
                                const baseappmgr::InformLoad& msg)
{
    auto it = baseapps_.find(src);
    if (it == baseapps_.end())
        return;
    it->second.load = msg.load;
    it->second.entity_count = msg.entity_count;
    it->second.proxy_count = msg.proxy_count;
}

void BaseAppMgr::on_allocate_baseapp(const Address& src, Channel* ch,
                                     const login::AllocateBaseApp& msg)
{
    login::AllocateBaseAppResult result;
    result.request_id = msg.request_id;

    if (is_overloaded())
    {
        ATLAS_LOG_WARNING("BaseAppMgr: overloaded, rejecting AllocateBaseApp req={}",
                          msg.request_id);
        result.success = false;
        (void)ch->send_message(result);
        return;
    }

    auto* best = find_least_loaded();
    if (!best)
    {
        ATLAS_LOG_WARNING("BaseAppMgr: no ready BaseApp available for req={}", msg.request_id);
        result.success = false;
        (void)ch->send_message(result);
        return;
    }

    result.success = true;
    result.internal_addr = best->internal_addr;
    result.external_addr = best->external_addr;
    (void)ch->send_message(result);

    ATLAS_LOG_DEBUG("BaseAppMgr: allocated BaseApp app_id={} for req={}", best->app_id,
                    msg.request_id);
    (void)src;
}

void BaseAppMgr::on_register_global_base(const Address& src, Channel* /*ch*/,
                                         const baseappmgr::RegisterGlobalBase& msg)
{
    GlobalBaseEntry entry;
    entry.key = msg.key;
    entry.base_addr = src;
    entry.entity_id = msg.entity_id;
    entry.type_id = msg.type_id;
    global_bases_[msg.key] = std::move(entry);

    baseappmgr::GlobalBaseNotification notif;
    notif.key = msg.key;
    notif.base_addr = src;
    notif.entity_id = msg.entity_id;
    notif.type_id = msg.type_id;
    notif.added = true;
    broadcast_to_all_baseapps(notif);

    ATLAS_LOG_INFO("BaseAppMgr: global base '{}' registered entity={}", msg.key, msg.entity_id);
}

void BaseAppMgr::on_deregister_global_base(const Address& /*src*/, Channel* /*ch*/,
                                           const baseappmgr::DeregisterGlobalBase& msg)
{
    auto it = global_bases_.find(msg.key);
    if (it == global_bases_.end())
        return;

    baseappmgr::GlobalBaseNotification notif;
    notif.key = msg.key;
    notif.base_addr = it->second.base_addr;
    notif.entity_id = it->second.entity_id;
    notif.type_id = it->second.type_id;
    notif.added = false;
    global_bases_.erase(it);
    broadcast_to_all_baseapps(notif);

    ATLAS_LOG_INFO("BaseAppMgr: global base '{}' deregistered", msg.key);
}

void BaseAppMgr::on_request_entity_id_range(const Address& src, Channel* ch,
                                            const baseappmgr::RequestEntityIdRange& msg)
{
    auto [start, end] = allocate_entity_id_range();

    baseappmgr::RequestEntityIdRangeAck ack;
    ack.app_id = msg.app_id;
    ack.entity_id_start = start;
    ack.entity_id_end = end;
    (void)ch->send_message(ack);

    ATLAS_LOG_INFO("BaseAppMgr: extended EntityID range [{},{}] to app_id={}", start, end,
                   msg.app_id);
    (void)src;
}

// ============================================================================
// Internal helpers
// ============================================================================

auto BaseAppMgr::find_least_loaded() const -> const BaseAppInfo*
{
    const BaseAppInfo* best = nullptr;
    float lowest = 2.0f;
    for (const auto& [addr, info] : baseapps_)
    {
        if (!info.is_ready || info.is_retiring)
            continue;
        if (info.load < lowest)
        {
            lowest = info.load;
            best = &info;
        }
    }
    return best;
}

auto BaseAppMgr::is_overloaded() const -> bool
{
    const auto* best = find_least_loaded();
    if (!best || best->load <= kOverloadThreshold)
    {
        overload_start_ = {};
        logins_since_overload_ = 0;
        return false;
    }

    auto now = Clock::now();
    if (overload_start_ == TimePoint{})
    {
        overload_start_ = now;
        logins_since_overload_ = 0;
    }

    auto duration = now - overload_start_;
    if (duration > std::chrono::seconds(5) || logins_since_overload_ >= kOverloadLoginLimit)
    {
        return true;
    }

    ++logins_since_overload_;
    return false;
}

void BaseAppMgr::broadcast_to_all_baseapps(const baseappmgr::GlobalBaseNotification& notif)
{
    for (auto& [addr, info] : baseapps_)
    {
        if (info.is_ready && info.channel)
            (void)info.channel->send_message(notif);
    }
}

auto BaseAppMgr::allocate_entity_id_range() -> std::pair<EntityID, EntityID>
{
    EntityID start = next_entity_range_start_;
    EntityID end = start + kEntityIdRangeSize - 1;
    next_entity_range_start_ = end + 1;
    return {start, end};
}

void BaseAppMgr::on_baseapp_death(const Address& addr)
{
    auto it = baseapps_.find(addr);
    if (it == baseapps_.end())
        return;
    ATLAS_LOG_WARNING("BaseAppMgr: BaseApp app_id={} died ({}:{})", it->second.app_id, addr.ip(),
                      addr.port());
    baseapps_.erase(it);

    // Clean up any global bases owned by the dead BaseApp
    for (auto git = global_bases_.begin(); git != global_bases_.end();)
    {
        if (git->second.base_addr == addr)
        {
            baseappmgr::GlobalBaseNotification notif;
            notif.key = git->second.key;
            notif.base_addr = addr;
            notif.entity_id = git->second.entity_id;
            notif.type_id = git->second.type_id;
            notif.added = false;
            broadcast_to_all_baseapps(notif);
            git = global_bases_.erase(git);
        }
        else
        {
            ++git;
        }
    }
}

}  // namespace atlas
