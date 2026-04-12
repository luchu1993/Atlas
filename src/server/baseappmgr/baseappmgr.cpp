#include "baseappmgr.hpp"

#include "foundation/log.hpp"
#include "foundation/time.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "server/watcher.hpp"

#include <algorithm>
#include <format>

namespace atlas
{

void BaseAppMgr::DbidAffinityTable::remember(DatabaseID dbid, uint32_t app_id, TimePoint now)
{
    if (dbid == kInvalidDBID || app_id == 0)
    {
        return;
    }

    if (auto existing = entries_.find(dbid); existing != entries_.end())
    {
        if (existing->second.app_id != app_id)
        {
            auto reverse = dbids_by_app_.find(existing->second.app_id);
            if (reverse != dbids_by_app_.end())
            {
                reverse->second.erase(dbid);
                if (reverse->second.empty())
                {
                    dbids_by_app_.erase(reverse);
                }
            }
        }
    }

    entries_[dbid] = Entry{app_id, now};
    dbids_by_app_[app_id].insert(dbid);
}

void BaseAppMgr::DbidAffinityTable::erase(DatabaseID dbid)
{
    auto it = entries_.find(dbid);
    if (it == entries_.end())
    {
        return;
    }

    if (auto reverse = dbids_by_app_.find(it->second.app_id); reverse != dbids_by_app_.end())
    {
        reverse->second.erase(dbid);
        if (reverse->second.empty())
        {
            dbids_by_app_.erase(reverse);
        }
    }

    entries_.erase(it);
}

void BaseAppMgr::DbidAffinityTable::forget_app(uint32_t app_id)
{
    auto reverse = dbids_by_app_.find(app_id);
    if (reverse == dbids_by_app_.end())
    {
        return;
    }

    for (DatabaseID dbid : reverse->second)
    {
        entries_.erase(dbid);
    }
    dbids_by_app_.erase(reverse);
}

void BaseAppMgr::DbidAffinityTable::prune_expired(TimePoint now, Duration ttl)
{
    for (auto it = entries_.begin(); it != entries_.end();)
    {
        if (now - it->second.last_assigned_at <= ttl)
        {
            ++it;
            continue;
        }

        if (auto reverse = dbids_by_app_.find(it->second.app_id); reverse != dbids_by_app_.end())
        {
            reverse->second.erase(it->first);
            if (reverse->second.empty())
            {
                dbids_by_app_.erase(reverse);
            }
        }
        it = entries_.erase(it);
    }
}

auto BaseAppMgr::DbidAffinityTable::find(DatabaseID dbid) const -> std::optional<Entry>
{
    auto it = entries_.find(dbid);
    if (it == entries_.end())
    {
        return std::nullopt;
    }
    return it->second;
}

auto BaseAppMgr::BaseAppInfo::queue_pressure() const -> float
{
    const float pressure_units =
        static_cast<float>(pending_prepare_count + pending_force_logoff_count +
                           deferred_login_count + logoff_in_flight_count) +
        static_cast<float>(detached_proxy_count) * 0.1f;

    // Queue depth is a balancing hint, not a hard overload signal. Scale it
    // conservatively so transient login bursts spread across BaseApps instead
    // of collapsing the whole cluster into "no_baseapp" rejections.
    return std::min(0.35f, pressure_units / 512.0f);
}

auto BaseAppMgr::BaseAppInfo::is_hard_overloaded(float overload_threshold) const -> bool
{
    return measured_load >= overload_threshold ||
           pending_prepare_count >= BaseAppMgr::kHardOverloadPendingPrepareLimit ||
           deferred_login_count >= BaseAppMgr::kHardOverloadDeferredLoginLimit ||
           (pending_force_logoff_count + logoff_in_flight_count) >=
               BaseAppMgr::kHardOverloadLogoffLimit;
}

void BaseAppMgr::BaseAppInfo::apply_load_report(
    float load, uint32_t reported_entity_count, uint32_t reported_proxy_count,
    uint32_t reported_pending_prepare_count, uint32_t reported_pending_force_logoff_count,
    uint32_t reported_detached_proxy_count, uint32_t reported_logoff_in_flight_count,
    uint32_t reported_deferred_login_count, TimePoint now)
{
    measured_load = std::clamp(load, 0.0f, 1.0f);
    entity_count = reported_entity_count;
    proxy_count = reported_proxy_count;
    pending_prepare_count = reported_pending_prepare_count;
    pending_force_logoff_count = reported_pending_force_logoff_count;
    detached_proxy_count = reported_detached_proxy_count;
    logoff_in_flight_count = reported_logoff_in_flight_count;
    deferred_login_count = reported_deferred_login_count;
    pending_login_allocations = 0;
    effective_load = std::clamp(std::max(measured_load, queue_pressure()), 0.0f, 1.0f);
    last_load_report_at = now;
}

void BaseAppMgr::BaseAppInfo::reserve_login_slot(float load_increment)
{
    ++pending_login_allocations;
    ++pending_prepare_count;
    ++entity_count;
    ++proxy_count;
    effective_load = std::min(1.0f, std::max(effective_load + load_increment, queue_pressure()));
}

auto BaseAppMgr::BaseAppInfo::has_fresh_load(TimePoint now, Duration stale_after) const -> bool
{
    if (last_load_report_at == TimePoint{})
    {
        return true;
    }

    return (now - last_load_report_at) <= stale_after;
}

namespace
{

auto resolve_advertised_addr(const Address& advertised, const Address& src) -> Address
{
    if (advertised.ip() != 0)
    {
        return advertised;
    }
    return Address(src.ip(), advertised.port());
}

}  // namespace

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
    wr.add<std::size_t>("baseappmgr/dbid_affinity_count",
                        std::function<std::size_t()>([this] { return dbid_affinity_.size(); }));
}

// ============================================================================
// Message handlers
// ============================================================================

void BaseAppMgr::on_register_baseapp(const Address& src, Channel* ch,
                                     const baseappmgr::RegisterBaseApp& msg)
{
    const Address internal_addr = resolve_advertised_addr(msg.internal_addr, src);
    const Address external_addr = resolve_advertised_addr(msg.external_addr, src);

    if (baseapps_.contains(internal_addr))
    {
        ATLAS_LOG_WARNING("BaseAppMgr: duplicate BaseApp registration for internal addr {}:{}",
                          internal_addr.ip(), internal_addr.port());

        baseappmgr::RegisterBaseAppAck ack;
        ack.success = false;
        (void)ch->send_message(ack);
        return;
    }

    auto [range_start, range_end] = allocate_entity_id_range();

    uint32_t app_id = next_app_id_++;
    BaseAppInfo info;
    info.internal_addr = internal_addr;
    info.external_addr = external_addr;
    info.app_id = app_id;
    info.channel = ch;
    const auto [it, inserted] = baseapps_.emplace(internal_addr, std::move(info));
    if (!inserted)
    {
        ATLAS_LOG_ERROR("BaseAppMgr: failed to insert BaseApp registration for {}:{}",
                        internal_addr.ip(), internal_addr.port());
        baseappmgr::RegisterBaseAppAck ack;
        ack.success = false;
        (void)ch->send_message(ack);
        return;
    }
    app_id_index_.emplace(app_id, it->first);

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
        app_id, internal_addr.ip(), internal_addr.port(), external_addr.ip(), external_addr.port(),
        range_start, range_end);
}

void BaseAppMgr::on_baseapp_ready(const Address& src, Channel* ch,
                                  const baseappmgr::BaseAppReady& msg)
{
    BaseAppInfo* info = find_baseapp_by_app_id(msg.app_id);
    if (info == nullptr)
    {
        ATLAS_LOG_WARNING("BaseAppMgr: BaseAppReady for unknown app_id={} from {}:{}", msg.app_id,
                          src.ip(), src.port());
        return;
    }

    if (!matches_registered_source(*info, src, ch, "BaseAppReady"))
        return;

    info->is_ready = true;
    ATLAS_LOG_INFO("BaseAppMgr: BaseApp app_id={} is ready", msg.app_id);
}

void BaseAppMgr::on_inform_load(const Address& src, Channel* ch, const baseappmgr::InformLoad& msg)
{
    BaseAppInfo* info = find_baseapp_by_app_id(msg.app_id);
    if (info == nullptr)
    {
        ATLAS_LOG_WARNING("BaseAppMgr: InformLoad for unknown app_id={} from {}:{}", msg.app_id,
                          src.ip(), src.port());
        return;
    }

    if (!matches_registered_source(*info, src, ch, "InformLoad"))
        return;

    info->apply_load_report(msg.load, msg.entity_count, msg.proxy_count, msg.pending_prepare_count,
                            msg.pending_force_logoff_count, msg.detached_proxy_count,
                            msg.logoff_in_flight_count, msg.deferred_login_count, Clock::now());
}

void BaseAppMgr::on_allocate_baseapp(const Address& src, Channel* ch,
                                     const login::AllocateBaseApp& msg)
{
    ATLAS_LOG_DEBUG("BaseAppMgr: allocate request_id={} type_id={} dbid={} from {}:{}",
                    msg.request_id, msg.type_id, msg.dbid, src.ip(), src.port());
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

    auto* best = find_allocation_target(msg.dbid);
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
    const auto send_result = ch->send_message(result);
    if (!send_result)
    {
        ATLAS_LOG_WARNING("BaseAppMgr: failed to reply AllocateBaseApp req={} app_id={}: {}",
                          msg.request_id, best->app_id, send_result.error().message());
        return;
    }

    record_successful_allocation(best->app_id, msg.dbid, Clock::now());

    ATLAS_LOG_DEBUG("BaseAppMgr: allocated BaseApp app_id={} for req={} dbid={}", best->app_id,
                    msg.request_id, msg.dbid);
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
    BaseAppInfo* info = find_baseapp_by_app_id(msg.app_id);
    if (info == nullptr)
    {
        ATLAS_LOG_WARNING("BaseAppMgr: RequestEntityIdRange for unknown app_id={} from {}:{}",
                          msg.app_id, src.ip(), src.port());
        return;
    }

    if (!matches_registered_source(*info, src, ch, "RequestEntityIdRange"))
        return;

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

auto BaseAppMgr::find_baseapp_by_app_id(uint32_t app_id) -> BaseAppInfo*
{
    const auto index_it = app_id_index_.find(app_id);
    if (index_it == app_id_index_.end())
        return nullptr;

    const auto it = baseapps_.find(index_it->second);
    if (it == baseapps_.end())
    {
        app_id_index_.erase(index_it);
        return nullptr;
    }

    return &it->second;
}

auto BaseAppMgr::find_baseapp_by_app_id(uint32_t app_id) const -> const BaseAppInfo*
{
    const auto index_it = app_id_index_.find(app_id);
    if (index_it == app_id_index_.end())
        return nullptr;

    const auto it = baseapps_.find(index_it->second);
    return (it != baseapps_.end()) ? &it->second : nullptr;
}

auto BaseAppMgr::matches_registered_source(const BaseAppInfo& info, const Address& src,
                                           const Channel* ch, std::string_view operation) const
    -> bool
{
    if (src != info.internal_addr)
    {
        ATLAS_LOG_WARNING("BaseAppMgr: {} source mismatch for app_id={} expected {}:{} got {}:{}",
                          operation, info.app_id, info.internal_addr.ip(),
                          info.internal_addr.port(), src.ip(), src.port());
        return false;
    }

    if (ch != nullptr && info.channel != nullptr && info.channel != ch)
    {
        ATLAS_LOG_WARNING("BaseAppMgr: {} channel mismatch for app_id={}", operation, info.app_id);
        return false;
    }

    return true;
}

auto BaseAppMgr::is_allocation_candidate(const BaseAppInfo& info, TimePoint now,
                                         Duration stale_after) const -> bool
{
    return info.is_ready && !info.is_retiring && info.has_fresh_load(now, stale_after);
}

auto BaseAppMgr::is_better_candidate(const BaseAppInfo& candidate, const BaseAppInfo& incumbent)
    -> bool
{
    if (candidate.effective_load != incumbent.effective_load)
    {
        return candidate.effective_load < incumbent.effective_load;
    }

    if (candidate.proxy_count != incumbent.proxy_count)
    {
        return candidate.proxy_count < incumbent.proxy_count;
    }

    if (candidate.entity_count != incumbent.entity_count)
    {
        return candidate.entity_count < incumbent.entity_count;
    }

    return candidate.app_id < incumbent.app_id;
}

auto BaseAppMgr::load_report_stale_after() const -> Duration
{
    const int update_hertz = std::max(config().update_hertz, 1);
    const auto expected_tick =
        std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / update_hertz));
    const auto minimum_staleness = std::chrono::duration_cast<Duration>(std::chrono::seconds(1));
    return std::max(expected_tick * 10, minimum_staleness);
}

auto BaseAppMgr::find_least_loaded() const -> const BaseAppInfo*
{
    const BaseAppInfo* best = nullptr;
    const auto now = Clock::now();
    const auto stale_after = load_report_stale_after();
    for (const auto& [addr, info] : baseapps_)
    {
        if (!is_allocation_candidate(info, now, stale_after))
        {
            continue;
        }

        if (best == nullptr || is_better_candidate(info, *best))
        {
            best = &info;
        }
    }
    return best;
}

auto BaseAppMgr::should_prefer_affinity(const BaseAppInfo& preferred,
                                        const BaseAppInfo* least_loaded) const -> bool
{
    if (preferred.is_hard_overloaded(kOverloadThreshold))
    {
        return false;
    }

    if (least_loaded == nullptr || preferred.app_id == least_loaded->app_id)
    {
        return true;
    }

    // Preserve DBID affinity using reported process load rather than the
    // queue-biased balancing score. For shortline relogin storms the preferred
    // BaseApp often carries more detached proxies precisely because it can
    // complete the reconnect locally; routing away from it defeats the fast
    // path and amplifies force-logoff pressure.
    const float allowed_load = std::min(1.0f, least_loaded->measured_load + kDbidAffinityLoadSlack);
    return preferred.measured_load <= allowed_load;
}

auto BaseAppMgr::find_allocation_target(DatabaseID dbid) -> const BaseAppInfo*
{
    const auto now = Clock::now();
    const auto stale_after = load_report_stale_after();
    dbid_affinity_.prune_expired(now, kDbidAffinityTtl);

    const BaseAppInfo* least_loaded = find_least_loaded();
    if (dbid == kInvalidDBID)
    {
        return least_loaded;
    }

    const auto affinity = dbid_affinity_.find(dbid);
    if (!affinity)
    {
        return least_loaded;
    }

    const auto* preferred = find_baseapp_by_app_id(affinity->app_id);
    if (preferred == nullptr || !is_allocation_candidate(*preferred, now, stale_after))
    {
        dbid_affinity_.erase(dbid);
        return least_loaded;
    }

    if (should_prefer_affinity(*preferred, least_loaded))
    {
        return preferred;
    }

    return least_loaded;
}

void BaseAppMgr::record_successful_allocation(uint32_t app_id, DatabaseID dbid, TimePoint now)
{
    if (auto* reserved = find_baseapp_by_app_id(app_id))
    {
        reserved->reserve_login_slot(kLoginAllocationLoadIncrement);
        dbid_affinity_.remember(dbid, reserved->app_id, now);
    }
}

auto BaseAppMgr::is_overloaded() const -> bool
{
    const auto* best = find_least_loaded();
    if (!best || !best->is_hard_overloaded(kOverloadThreshold))
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
    dbid_affinity_.forget_app(it->second.app_id);
    app_id_index_.erase(it->second.app_id);
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
