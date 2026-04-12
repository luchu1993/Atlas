#include "baseapp.hpp"

#include "baseapp_messages.hpp"
#include "baseapp_native_provider.hpp"
#include "baseappmgr/baseappmgr_messages.hpp"
#include "db/idatabase.hpp"
#include "dbapp/dbapp_messages.hpp"
#include "entitydef/entity_def_registry.hpp"
#include "foundation/log.hpp"
#include "loginapp/login_messages.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "network/reliable_udp.hpp"
#include "script/script_value.hpp"
#include "server/watcher.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <limits>
#include <span>
#include <vector>

namespace atlas
{

namespace
{

auto has_managed_entity_type(uint16_t type_id) -> bool
{
    return EntityDefRegistry::instance().find_by_id(type_id) != nullptr;
}

}  // namespace

void BaseApp::LoadTracker::mark_tick_started()
{
    tick_started_ = Clock::now();
}

void BaseApp::LoadTracker::observe_tick_complete(int update_hertz, const LoadSnapshot& snapshot)
{
    if (tick_started_ == TimePoint{})
    {
        return;
    }

    const int safe_update_hertz = std::max(update_hertz, 1);
    const auto work_duration = Clock::now() - tick_started_;
    const auto expected_tick = std::chrono::duration<double>(1.0 / safe_update_hertz);
    const float instantaneous =
        std::clamp(static_cast<float>(work_duration / expected_tick), 0.0f, 1.0f);
    const float queue_pressure_units =
        static_cast<float>(snapshot.pending_prepare_count + snapshot.deferred_login_count) +
        static_cast<float>(snapshot.pending_force_logoff_count + snapshot.logoff_in_flight_count) *
            0.5f +
        static_cast<float>(snapshot.detached_proxy_count) * 0.1f;
    const float queue_pressure = std::min(0.35f, queue_pressure_units / 512.0f);
    const float target = std::clamp(instantaneous + queue_pressure, 0.0f, 1.0f);
    load_ += (target - load_) * BaseApp::kLoadSmoothingBias;
}

auto BaseApp::LoadTracker::build_report(uint32_t app_id, const LoadSnapshot& snapshot) const
    -> baseappmgr::InformLoad
{
    baseappmgr::InformLoad msg;
    msg.app_id = app_id;
    msg.load = load_;
    msg.entity_count = snapshot.entity_count;
    msg.proxy_count = snapshot.proxy_count;
    msg.pending_prepare_count = snapshot.pending_prepare_count;
    msg.pending_force_logoff_count = snapshot.pending_force_logoff_count;
    msg.detached_proxy_count = snapshot.detached_proxy_count;
    msg.logoff_in_flight_count = snapshot.logoff_in_flight_count;
    msg.deferred_login_count = snapshot.deferred_login_count;
    return msg;
}

// ============================================================================
// run — static entry point
// ============================================================================

auto BaseApp::run(int argc, char* argv[]) -> int
{
    EventDispatcher dispatcher;
    NetworkInterface internal_network(dispatcher);
    NetworkInterface external_network(dispatcher);
    BaseApp app(dispatcher, internal_network, external_network);
    return app.run_app(argc, argv);
}

BaseApp::BaseApp(EventDispatcher& dispatcher, NetworkInterface& internal_network,
                 NetworkInterface& external_network)
    : EntityApp(dispatcher, internal_network), external_network_(external_network)
{
}

BaseApp::~BaseApp() = default;

// ============================================================================
// init
// ============================================================================

auto BaseApp::init(int argc, char* argv[]) -> bool
{
    if (!EntityApp::init(argc, argv))
        return false;

    const auto& cfg = config();

    // Derive app_index from port offset or machined registration order.
    // For now we use a simple heuristic: (external_port / 1000) % 1000.
    uint32_t app_index =
        (cfg.external_port > 0) ? static_cast<uint32_t>(cfg.external_port / 1000) % 1000 : 0;
    entity_mgr_ = EntityManager(app_index);

    // ---- Register internal message handlers --------------------------------
    register_internal_handlers();

    // ---- Open external RUDP listener (client-facing) -----------------------
    if (cfg.external_port > 0)
    {
        Address ext_addr(0, cfg.external_port);
        auto listen_result = external_network_.start_rudp_server(
            ext_addr, NetworkInterface::internet_rudp_profile());
        if (!listen_result)
        {
            ATLAS_LOG_ERROR("BaseApp: failed to listen on external port {}: {}", cfg.external_port,
                            listen_result.error().message());
            return false;
        }
        ATLAS_LOG_INFO("BaseApp: external RUDP interface listening on port {}", cfg.external_port);
    }
    external_network_.set_disconnect_callback([this](Channel& ch)
                                              { on_external_client_disconnect(ch); });

    // ---- Subscribe to DBApp birth notification to connect ----------------
    machined_client().subscribe(
        machined::ListenerType::Both, ProcessType::DBApp,
        [this](const machined::BirthNotification& n)
        {
            if (dbapp_channel_ == nullptr)
            {
                ATLAS_LOG_INFO("BaseApp: DBApp born at {}:{}, connecting via RUDP...",
                               n.internal_addr.ip(), n.internal_addr.port());
                auto ch = network().connect_rudp_nocwnd(n.internal_addr);
                if (ch)
                    dbapp_channel_ = static_cast<Channel*>(*ch);
            }
        },
        [this](const machined::DeathNotification& n)
        {
            (void)n;
            ATLAS_LOG_WARNING("BaseApp: DBApp died, clearing dbapp channel");
            dbapp_channel_ = nullptr;
            fail_all_dbapp_pending_requests("dbapp_disconnected");
        });

    // ---- Subscribe to BaseAppMgr and register ourselves ----------------
    machined_client().subscribe(
        machined::ListenerType::Birth, ProcessType::BaseAppMgr,
        [this](const machined::BirthNotification& n)
        {
            if (baseappmgr_channel_ == nullptr)
            {
                ATLAS_LOG_INFO("BaseApp: BaseAppMgr born at {}:{}, registering via RUDP...",
                               n.internal_addr.ip(), n.internal_addr.port());
                auto ch = network().connect_rudp_nocwnd(n.internal_addr);
                if (!ch)
                {
                    ATLAS_LOG_ERROR("BaseApp: failed to connect to BaseAppMgr");
                    return;
                }
                baseappmgr_channel_ = static_cast<Channel*>(*ch);

                // Register ourselves — advertise the RUDP internal address so
                // LoginApp and other peers can connect_rudp() to reach us.
                baseappmgr::RegisterBaseApp reg;
                reg.internal_addr = network().rudp_address();
                reg.external_addr =
                    Address(network().rudp_address().ip(), external_network_.rudp_address().port());
                (void)baseappmgr_channel_->send_message(reg);
            }
        },
        nullptr);

    // ---- Register login-flow internal message handlers ------------------
    auto& table = network().interface_table();
    (void)table.register_typed_handler<login::PrepareLogin>(
        [this](const Address& /*src*/, Channel* ch, const login::PrepareLogin& msg)
        { on_prepare_login(*ch, msg); });
    (void)table.register_typed_handler<login::CancelPrepareLogin>(
        [this](const Address& /*src*/, Channel* ch, const login::CancelPrepareLogin& msg)
        { on_cancel_prepare_login(*ch, msg); });
    (void)table.register_typed_handler<baseapp::ForceLogoff>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::ForceLogoff& msg)
        { on_force_logoff(*ch, msg); });
    (void)table.register_typed_handler<baseapp::ForceLogoffAck>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::ForceLogoffAck& msg)
        { on_force_logoff_ack(*ch, msg); });
    (void)table.register_typed_handler<baseappmgr::RegisterBaseAppAck>(
        [this](const Address& /*src*/, Channel* ch, const baseappmgr::RegisterBaseAppAck& msg)
        { on_register_baseapp_ack(*ch, msg); });
    (void)table.register_typed_handler<baseappmgr::RequestEntityIdRangeAck>(
        [this](const Address& /*src*/, Channel* ch, const baseappmgr::RequestEntityIdRangeAck& msg)
        { on_request_entity_id_range_ack(*ch, msg); });

    // ---- Register external client handler -------------------------------
    auto& ext_table = external_network_.interface_table();
    (void)ext_table.register_typed_handler<baseapp::Authenticate>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::Authenticate& msg)
        { on_client_authenticate(*ch, msg); });

    ATLAS_LOG_INFO("BaseApp: initialised (app_index={})", app_index);
    return true;
}

// ============================================================================
// fini
// ============================================================================

void BaseApp::fini()
{
    entity_mgr_.for_each(
        [this](const BaseEntity& ent)
        {
            if (ent.dbid() != kInvalidDBID)
                release_checkout(ent.dbid(), ent.type_id());
        });
    entity_mgr_.flush_destroyed();
    EntityApp::fini();
}

// ============================================================================
// on_tick_complete
// ============================================================================

void BaseApp::on_end_of_tick()
{
    load_tracker_.mark_tick_started();
}

void BaseApp::on_tick_complete()
{
    entity_mgr_.flush_destroyed();
    entity_mgr_.cleanup_retired_sessions();
    expire_detached_proxies();
    update_load_estimate();
    report_load_to_baseappmgr();
    cleanup_expired_pending_requests();
    maybe_request_more_ids();

    EntityApp::on_tick_complete();
}

// ============================================================================
// create_native_provider
// ============================================================================

auto BaseApp::create_native_provider() -> std::unique_ptr<INativeApiProvider>
{
    auto provider = std::make_unique<BaseAppNativeProvider>(*this);
    native_provider_ = provider.get();
    return provider;
}

// ============================================================================
// register_watchers
// ============================================================================

void BaseApp::register_watchers()
{
    EntityApp::register_watchers();
    auto& wr = watcher_registry();
    wr.add<float>("baseapp/load",
                  std::function<float()>([this] { return load_tracker_.current_load(); }));
    wr.add<std::size_t>("baseapp/entity_count",
                        std::function<std::size_t()>([this] { return entity_mgr_.size(); }));
    wr.add<std::size_t>("baseapp/proxy_count",
                        std::function<std::size_t()>([this] { return entity_mgr_.proxy_count(); }));
    wr.add<std::size_t>(
        "baseapp/client_binding_count",
        std::function<std::size_t()>([this] { return entity_client_index_.size(); }));
    wr.add<uint64_t>("baseapp/auth_success_total",
                     std::function<uint64_t()>([this] { return auth_success_total_; }));
    wr.add<uint64_t>("baseapp/auth_fail_total",
                     std::function<uint64_t()>([this] { return auth_fail_total_; }));
    wr.add<uint64_t>("baseapp/force_logoff_total",
                     std::function<uint64_t()>([this] { return force_logoff_total_; }));
    wr.add<uint64_t>("baseapp/fast_relogin_total",
                     std::function<uint64_t()>([this] { return fast_relogin_total_; }));
    wr.add<uint64_t>("baseapp/detached_relogin_total",
                     std::function<uint64_t()>([this] { return detached_relogin_total_; }));
    wr.add<std::size_t>("baseapp/pending_prepare_count",
                        std::function<std::size_t()>([this] { return pending_logins_.size(); }));
    wr.add<std::size_t>(
        "baseapp/pending_force_logoff_count",
        std::function<std::size_t()>([this] { return pending_force_logoffs_.size(); }));
    wr.add<std::size_t>(
        "baseapp/deferred_login_checkout_count",
        std::function<std::size_t()>([this] { return deferred_login_checkout_count(); }));
    wr.add<std::size_t>("baseapp/detached_proxy_count",
                        std::function<std::size_t()>([this] { return detached_proxies_.size(); }));
    wr.add<std::size_t>(
        "baseapp/logoff_in_flight_count",
        std::function<std::size_t()>([this] { return logoff_entities_in_flight_.size(); }));
    wr.add<bool>("baseapp/dbapp_connected",
                 std::function<bool()>([this] { return dbapp_channel_ != nullptr; }));
}

// ============================================================================
// register_internal_handlers
// ============================================================================

void BaseApp::register_internal_handlers()
{
    auto& table = network().interface_table();

    (void)table.register_typed_handler<baseapp::CreateBase>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::CreateBase& msg)
        { on_create_base(*ch, msg); });

    (void)table.register_typed_handler<baseapp::CreateBaseFromDB>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::CreateBaseFromDB& msg)
        { on_create_base_from_db(*ch, msg); });

    (void)table.register_typed_handler<baseapp::AcceptClient>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::AcceptClient& msg)
        { on_accept_client(*ch, msg); });

    (void)table.register_typed_handler<baseapp::CellEntityCreated>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::CellEntityCreated& msg)
        { on_cell_entity_created(*ch, msg); });

    (void)table.register_typed_handler<baseapp::CellEntityDestroyed>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::CellEntityDestroyed& msg)
        { on_cell_entity_destroyed(*ch, msg); });

    (void)table.register_typed_handler<baseapp::CurrentCell>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::CurrentCell& msg)
        { on_current_cell(*ch, msg); });

    (void)table.register_typed_handler<baseapp::CellRpcForward>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::CellRpcForward& msg)
        { on_cell_rpc_forward(*ch, msg); });

    (void)table.register_typed_handler<baseapp::SelfRpcFromCell>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::SelfRpcFromCell& msg)
        { on_self_rpc_from_cell(*ch, msg); });

    (void)table.register_typed_handler<baseapp::BroadcastRpcFromCell>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::BroadcastRpcFromCell& msg)
        { on_broadcast_rpc_from_cell(*ch, msg); });

    (void)table.register_typed_handler<baseapp::ReplicatedDeltaFromCell>(
        [this](const Address& /*src*/, Channel* ch, const baseapp::ReplicatedDeltaFromCell& msg)
        { on_replicated_delta_from_cell(*ch, msg); });

    // ---- WriteEntityAck (DBApp → BaseApp) ----------------------------------
    (void)table.register_typed_handler<dbapp::WriteEntityAck>(
        [this](const Address& /*src*/, Channel* /*ch*/, const dbapp::WriteEntityAck& msg)
        {
            auto logoff_it = pending_logoff_writes_.find(msg.request_id);
            if (logoff_it != pending_logoff_writes_.end())
            {
                const PendingLogoffWrite pending_write = logoff_it->second;
                pending_logoff_writes_.erase(logoff_it);
                logoff_entities_in_flight_.erase(pending_write.entity_id);

                if (!msg.success)
                {
                    ATLAS_LOG_ERROR(
                        "BaseApp: logoff persist failed request_id={} entity_id={} "
                        "dbid={} error={}",
                        msg.request_id, pending_write.entity_id, pending_write.dbid, msg.error);

                    if (pending_write.continuation_request_id != 0)
                    {
                        fail_pending_force_logoff(pending_write.continuation_request_id,
                                                  "force_logoff_persist_failed");
                    }
                    if (auto waiter_it =
                            pending_local_force_logoff_waiters_.find(pending_write.entity_id);
                        waiter_it != pending_local_force_logoff_waiters_.end())
                    {
                        for (uint32_t waiter_request_id : waiter_it->second)
                        {
                            fail_pending_force_logoff(waiter_request_id,
                                                      "force_logoff_persist_failed");
                        }
                        pending_local_force_logoff_waiters_.erase(waiter_it);
                    }
                    fail_deferred_prepare_logins(pending_write.entity_id,
                                                 "force_logoff_persist_failed");
                    flush_remote_force_logoff_acks(pending_write.entity_id, false);
                    finish_login_flow(pending_write.dbid);
                    return;
                }

                if (!finalize_force_logoff(pending_write.entity_id))
                {
                    if (pending_write.continuation_request_id != 0)
                    {
                        fail_pending_force_logoff(pending_write.continuation_request_id,
                                                  "force_logoff_destroy_failed");
                    }
                    if (auto waiter_it =
                            pending_local_force_logoff_waiters_.find(pending_write.entity_id);
                        waiter_it != pending_local_force_logoff_waiters_.end())
                    {
                        for (uint32_t waiter_request_id : waiter_it->second)
                        {
                            fail_pending_force_logoff(waiter_request_id,
                                                      "force_logoff_destroy_failed");
                        }
                        pending_local_force_logoff_waiters_.erase(waiter_it);
                    }
                    fail_deferred_prepare_logins(pending_write.entity_id,
                                                 "force_logoff_destroy_failed");
                    flush_remote_force_logoff_acks(pending_write.entity_id, false);
                    finish_login_flow(pending_write.dbid);
                    return;
                }

                const bool resumed_deferred =
                    resume_deferred_prepare_logins(pending_write.entity_id);

                if (auto waiter_it =
                        pending_local_force_logoff_waiters_.find(pending_write.entity_id);
                    waiter_it != pending_local_force_logoff_waiters_.end())
                {
                    for (uint32_t waiter_request_id : waiter_it->second)
                    {
                        if (pending_force_logoffs_.contains(waiter_request_id))
                        {
                            continue_login_after_force_logoff(waiter_request_id);
                        }
                    }
                    pending_local_force_logoff_waiters_.erase(waiter_it);
                }

                flush_remote_force_logoff_acks(pending_write.entity_id, true);

                if (pending_write.continuation_request_id != 0 &&
                    pending_force_logoffs_.contains(pending_write.continuation_request_id))
                {
                    continue_login_after_force_logoff(pending_write.continuation_request_id);
                }
                else if (!resumed_deferred)
                {
                    finish_login_flow(pending_write.dbid);
                }
                return;
            }

            auto* ent = entity_mgr_.find(msg.request_id);
            if (ent)
            {
                if (msg.success && msg.dbid != kInvalidDBID)
                {
                    (void)entity_mgr_.assign_dbid(ent->entity_id(), msg.dbid);
                }
                ent->on_write_ack(msg.dbid, msg.success);
            }
        });

    // ---- CheckoutEntityAck (DBApp → BaseApp) --------------------------------
    (void)table.register_typed_handler<dbapp::CheckoutEntityAck>(
        [this](const Address& /*src*/, Channel* /*ch*/, const dbapp::CheckoutEntityAck& msg)
        {
            if (auto canceled_it = canceled_login_checkouts_.find(msg.request_id);
                canceled_it != canceled_login_checkouts_.end())
            {
                if (msg.status == dbapp::CheckoutStatus::Success && msg.dbid != kInvalidDBID)
                {
                    release_checkout(msg.dbid, canceled_it->second.type_id);
                }
                canceled_login_checkouts_.erase(canceled_it);
                return;
            }

            // Check if this is a login-flow checkout (request_id is a pending key)
            auto login_it = pending_logins_.find(msg.request_id);
            if (login_it != pending_logins_.end())
            {
                PendingLogin pending = login_it->second;
                pending_logins_.erase(login_it);

                if (msg.status != dbapp::CheckoutStatus::Success)
                {
                    if (msg.status == dbapp::CheckoutStatus::AlreadyCheckedOut &&
                        retry_login_after_checkout_conflict(std::move(pending), msg.dbid,
                                                            msg.holder_addr))
                    {
                        return;
                    }

                    login::PrepareLoginResult reply;
                    reply.request_id = pending.login_request_id;
                    ATLAS_LOG_ERROR(
                        "BaseApp: login checkout failed (request_id={} status={} holder={}:{})",
                        msg.request_id, static_cast<int>(msg.status), msg.holder_addr.ip(),
                        msg.holder_addr.port());
                    reply.success = false;
                    reply.error = (msg.status == dbapp::CheckoutStatus::AlreadyCheckedOut)
                                      ? "already_checked_out"
                                      : (msg.error.empty() ? "checkout_failed" : msg.error);
                    if (!pending.reply_sent)
                    {
                        send_prepare_login_result(pending.loginapp_addr, reply);
                    }
                    finish_login_flow(pending.dbid);
                    return;
                }

                complete_prepare_login_from_checkout(
                    std::move(pending), msg.dbid, pending.type_id,
                    std::span<const std::byte>(reinterpret_cast<const std::byte*>(msg.blob.data()),
                                               msg.blob.size()));
                return;
            }

            // Fallback: CreateBaseFromDB checkout — request_id == entity_id
            const EntityID eid = msg.request_id;
            if (msg.status != dbapp::CheckoutStatus::Success)
            {
                ATLAS_LOG_ERROR("BaseApp: checkout failed (entity_id={} status={})", eid,
                                static_cast<int>(msg.status));
                entity_mgr_.destroy(eid);
                return;
            }
            auto* ent = entity_mgr_.find(eid);
            if (!ent)
                return;
            ent->set_entity_data(std::vector<std::byte>(msg.blob.begin(), msg.blob.end()));
            if (!restore_managed_entity(
                    eid, ent->type_id(), msg.dbid,
                    std::span<const std::byte>(msg.blob.data(), msg.blob.size())))
            {
                (void)notify_managed_entity_destroyed(eid, "CreateBaseFromDB rollback");
                entity_mgr_.destroy(eid);
                release_checkout(msg.dbid, ent->type_id());
            }
        });

    (void)table.register_typed_handler<dbapp::AbortCheckoutAck>(
        [this](const Address& /*src*/, Channel* /*ch*/, const dbapp::AbortCheckoutAck& msg)
        { canceled_login_checkouts_.erase(msg.request_id); });
}

// ============================================================================
// Message handlers
// ============================================================================

void BaseApp::on_create_base(Channel& /*ch*/, const baseapp::CreateBase& msg)
{
    const auto& defs = entity_defs();
    auto* type = defs.find_by_id(msg.type_id);
    if (!type)
    {
        ATLAS_LOG_ERROR("BaseApp: CreateBase: unknown type_id {}", msg.type_id);
        return;
    }
    bool has_client = type->has_client;
    auto* ent = entity_mgr_.create(msg.type_id, has_client);
    if (!ent)
    {
        ATLAS_LOG_ERROR("BaseApp: CreateBase: EntityID range exhausted for type_id {}",
                        msg.type_id);
        return;
    }

    ATLAS_LOG_DEBUG("BaseApp: created entity id={} type={}", ent->entity_id(), msg.type_id);
}

void BaseApp::on_create_base_from_db(Channel& /*ch*/, const baseapp::CreateBaseFromDB& msg)
{
    if (!dbapp_channel_)
    {
        ATLAS_LOG_ERROR("BaseApp: CreateBaseFromDB: no DBApp connection");
        return;
    }
    const auto& defs = entity_defs();
    auto* type = defs.find_by_id(msg.type_id);
    bool has_client = type ? type->has_client : false;
    auto* ent = entity_mgr_.create(msg.type_id, has_client, msg.dbid);
    if (!ent)
    {
        ATLAS_LOG_ERROR(
            "BaseApp: CreateBaseFromDB: EntityID range exhausted for type_id {} dbid {}",
            msg.type_id, msg.dbid);
        return;
    }

    dbapp::CheckoutEntity req;
    req.mode = msg.identifier.empty() ? dbapp::LoadMode::ByDBID : dbapp::LoadMode::ByName;
    req.type_id = msg.type_id;
    req.dbid = msg.dbid;
    req.identifier = msg.identifier;
    req.entity_id = ent->entity_id();
    (void)dbapp_channel_->send_message(req);
}

void BaseApp::on_accept_client(Channel& /*ch*/, const baseapp::AcceptClient& msg)
{
    auto* proxy = entity_mgr_.find_proxy(msg.dest_entity_id);
    if (!proxy)
    {
        ATLAS_LOG_WARNING("BaseApp: AcceptClient: entity {} not a Proxy", msg.dest_entity_id);
        return;
    }
    if (!entity_mgr_.assign_session_key(proxy->entity_id(), msg.session_key))
    {
        ATLAS_LOG_ERROR("BaseApp: AcceptClient: failed to bind session key to entity {}",
                        msg.dest_entity_id);
        return;
    }
    clear_detached_grace(proxy->entity_id());
    ATLAS_LOG_DEBUG("BaseApp: entity {} ready for client", msg.dest_entity_id);
}

void BaseApp::on_cell_entity_created(Channel& /*ch*/, const baseapp::CellEntityCreated& msg)
{
    auto* ent = entity_mgr_.find(msg.base_entity_id);
    if (!ent)
        return;
    ent->set_cell(msg.cell_entity_id, msg.cell_addr);
    ATLAS_LOG_DEBUG("BaseApp: entity {} has cell at {}:{}", msg.base_entity_id, msg.cell_addr.ip(),
                    msg.cell_addr.port());
}

void BaseApp::on_cell_entity_destroyed(Channel& /*ch*/, const baseapp::CellEntityDestroyed& msg)
{
    auto* ent = entity_mgr_.find(msg.base_entity_id);
    if (!ent)
        return;
    ent->clear_cell();
}

void BaseApp::on_current_cell(Channel& /*ch*/, const baseapp::CurrentCell& msg)
{
    auto* ent = entity_mgr_.find(msg.base_entity_id);
    if (!ent)
        return;
    ent->set_cell(msg.cell_entity_id, msg.cell_addr);
}

void BaseApp::on_cell_rpc_forward(Channel& /*ch*/, const baseapp::CellRpcForward& msg)
{
    // C# entity receives the base-directed RPC call via the native API callback table.
    // We invoke the RPC handler through the script engine.
    std::vector<ScriptValue> ba_args{
        ScriptValue::from_int(static_cast<int32_t>(msg.base_entity_id)),
        ScriptValue::from_int(static_cast<int32_t>(msg.rpc_id)),
        ScriptValue(std::vector<std::byte>(msg.payload.begin(), msg.payload.end()))};
    (void)script_engine().call_function("Atlas.Runtime", "OnBaseRpc", ba_args);
}

void BaseApp::on_self_rpc_from_cell(Channel& /*ch*/, const baseapp::SelfRpcFromCell& msg)
{
    auto* proxy = entity_mgr_.find_proxy(msg.base_entity_id);
    if (!proxy || !proxy->has_client())
        return;

    if (auto* client_ch = resolve_client_channel(proxy->entity_id()))
    {
        (void)client_ch->send_message(
            static_cast<MessageID>(msg.rpc_id),
            std::span<const std::byte>(msg.payload.data(), msg.payload.size()));
    }
}

void BaseApp::on_broadcast_rpc_from_cell(Channel& /*ch*/, const baseapp::BroadcastRpcFromCell& msg)
{
    auto* proxy = entity_mgr_.find_proxy(msg.base_entity_id);
    if (!proxy || !proxy->has_client())
        return;

    if (auto* client_ch = resolve_client_channel(proxy->entity_id()))
    {
        (void)client_ch->send_message(
            static_cast<MessageID>(msg.rpc_id),
            std::span<const std::byte>(msg.payload.data(), msg.payload.size()));
    }
}

void BaseApp::on_replicated_delta_from_cell(Channel& /*ch*/,
                                            const baseapp::ReplicatedDeltaFromCell& msg)
{
    auto* proxy = entity_mgr_.find_proxy(msg.base_entity_id);
    if (!proxy || !proxy->has_client())
        return;

    // Forward replicated delta to client (reserved update message ID)
    if (auto* client_ch = resolve_client_channel(proxy->entity_id()))
    {
        (void)client_ch->send_message(
            static_cast<MessageID>(0xF001),
            std::span<const std::byte>(msg.delta.data(), msg.delta.size()));
    }
}

// ============================================================================
// NativeProvider bridging
// ============================================================================

void BaseApp::do_write_to_db(EntityID entity_id, const std::byte* data, int32_t len)
{
    if (!dbapp_channel_)
    {
        ATLAS_LOG_ERROR("BaseApp: write_to_db: no DBApp connection (entity={})", entity_id);
        return;
    }
    auto* ent = entity_mgr_.find(entity_id);
    if (!ent)
        return;

    dbapp::WriteEntity msg;
    // Use CreateNew for first save (dbid=0), ExplicitDBID for updates
    msg.flags = (ent->dbid() == kInvalidDBID) ? WriteFlags::CreateNew : WriteFlags::ExplicitDBID;
    msg.type_id = ent->type_id();
    msg.dbid = ent->dbid();
    msg.entity_id = entity_id;
    msg.request_id = entity_id;  // echo back in WriteEntityAck
    msg.blob.assign(data, data + len);
    (void)dbapp_channel_->send_message(msg);
}

auto BaseApp::capture_entity_snapshot(EntityID entity_id, std::vector<std::byte>& out) -> bool
{
    auto* ent = entity_mgr_.find(entity_id);
    if (!ent)
    {
        return false;
    }

    if (has_managed_entity_type(ent->type_id()) && native_provider_)
    {
        if (auto fn = native_provider_->get_entity_data_fn())
        {
            uint8_t* raw = nullptr;
            int32_t len = 0;
            clear_native_api_error();
            fn(entity_id, &raw, &len);

            if (auto error = consume_native_api_error())
            {
                ATLAS_LOG_ERROR("BaseApp: get_entity_data failed for entity={}: {}", entity_id,
                                *error);
                return false;
            }

            if (len < 0)
            {
                ATLAS_LOG_ERROR("BaseApp: get_entity_data returned negative length for entity={}",
                                entity_id);
                return false;
            }
            if (len > 0 && raw == nullptr)
            {
                ATLAS_LOG_ERROR(
                    "BaseApp: get_entity_data returned null payload for entity={} len={}",
                    entity_id, len);
                return false;
            }
            if (len == 0)
            {
                out.clear();
                ent->set_entity_data({});
                return true;
            }

            out.assign(reinterpret_cast<const std::byte*>(raw),
                       reinterpret_cast<const std::byte*>(raw) + len);
            ent->set_entity_data(out);
            return true;
        }
    }

    out = ent->entity_data();
    if (has_managed_entity_type(ent->type_id()))
    {
        ATLAS_LOG_WARNING("BaseApp: no managed snapshot callback for entity={}, using cached blob",
                          entity_id);
    }
    return true;
}

auto BaseApp::rotate_proxy_session(EntityID entity_id, const SessionKey& session_key) -> bool
{
    auto* proxy = entity_mgr_.find_proxy(entity_id);
    if (!proxy)
    {
        return false;
    }

    proxy->bump_session_epoch();
    return entity_mgr_.assign_session_key(entity_id, session_key);
}

void BaseApp::enter_detached_grace(EntityID entity_id)
{
    auto* proxy = entity_mgr_.find_proxy(entity_id);
    if (!proxy || proxy->dbid() == kInvalidDBID)
    {
        return;
    }

    const auto now = Clock::now();
    const auto until = now + kDetachedProxyGrace;
    proxy->enter_detached_grace(until);
    detached_proxies_[entity_id] = DetachedProxyState{now, until};
}

void BaseApp::clear_detached_grace(EntityID entity_id)
{
    detached_proxies_.erase(entity_id);
    if (auto* proxy = entity_mgr_.find_proxy(entity_id))
    {
        proxy->clear_detached_grace();
    }
}

void BaseApp::expire_detached_proxies()
{
    const auto now = Clock::now();
    for (auto it = detached_proxies_.begin(); it != detached_proxies_.end();)
    {
        const EntityID entity_id = it->first;
        auto* proxy = entity_mgr_.find_proxy(entity_id);
        if (!proxy || proxy->has_client())
        {
            if (proxy)
            {
                proxy->clear_detached_grace();
            }
            it = detached_proxies_.erase(it);
            continue;
        }

        if (now < it->second.detached_until)
        {
            ++it;
            continue;
        }

        proxy->clear_detached_grace();
        it = detached_proxies_.erase(it);
        start_disconnect_logoff(entity_id);
    }
}

auto BaseApp::deferred_login_checkout_count() const -> std::size_t
{
    std::size_t total = 0;
    for (const auto& [entity_id, deferred] : deferred_login_checkouts_)
    {
        (void)entity_id;
        total += deferred.size();
    }
    return total;
}

auto BaseApp::try_complete_local_relogin(PendingLogin pending) -> bool
{
    if (queued_logins_.contains(pending.dbid))
    {
        fail_pending_prepare_login(pending, "superseded_by_newer_login");
        finish_login_flow(pending.dbid);
        return true;
    }

    auto* ent = entity_mgr_.find_by_dbid(pending.dbid);
    if (!ent || ent->is_pending_destroy())
    {
        return false;
    }

    auto* proxy = entity_mgr_.find_proxy(ent->entity_id());
    if (!proxy)
    {
        return false;
    }

    const bool was_detached = proxy->is_detached();
    if (auto* existing_client = resolve_client_channel(proxy->entity_id()))
    {
        existing_client->condemn();
    }
    unbind_client(proxy->entity_id());
    clear_detached_grace(proxy->entity_id());

    if (!rotate_proxy_session(proxy->entity_id(), pending.session_key))
    {
        fail_pending_prepare_login(pending, "session_conflict");
        finish_login_flow(pending.dbid);
        return true;
    }

    login::PrepareLoginResult reply;
    reply.request_id = pending.login_request_id;
    reply.success = true;
    reply.entity_id = proxy->entity_id();
    send_prepare_login_result(pending.loginapp_addr, reply);

    ++fast_relogin_total_;
    if (was_detached)
    {
        ++detached_relogin_total_;
    }

    ATLAS_LOG_DEBUG("BaseApp: fast relogin entity={} dbid={} detached={} epoch={}",
                    proxy->entity_id(), pending.dbid, was_detached ? 1 : 0, proxy->session_epoch());
    finish_login_flow(pending.dbid);
    return true;
}

void BaseApp::complete_prepare_login_from_checkout(PendingLogin pending, DatabaseID dbid,
                                                   uint16_t type_id,
                                                   std::span<const std::byte> blob)
{
    login::PrepareLoginResult reply;
    reply.request_id = pending.login_request_id;

    if (pending.reply_sent)
    {
        release_checkout(dbid, type_id);
        finish_login_flow(pending.dbid);
        return;
    }

    if (queued_logins_.contains(pending.dbid))
    {
        release_checkout(dbid, type_id);
        fail_pending_prepare_login(pending, "superseded_by_newer_login");
        finish_login_flow(pending.dbid);
        return;
    }

    if (auto* blocking_ent = entity_mgr_.find_by_dbid(dbid); blocking_ent)
    {
        defer_prepare_login_from_checkout(blocking_ent->entity_id(), std::move(pending), dbid,
                                          type_id, blob);
        start_disconnect_logoff(blocking_ent->entity_id());
        return;
    }

    auto* ent = entity_mgr_.create(type_id, true);
    if (!ent)
    {
        ATLAS_LOG_ERROR("BaseApp: failed to allocate EntityID for login entity type={} dbid={}",
                        type_id, dbid);
        release_checkout(dbid, type_id);
        reply.success = false;
        reply.error = "entity_id_exhausted";
        send_prepare_login_result(pending.loginapp_addr, reply);
        finish_login_flow(pending.dbid);
        return;
    }

    ent->set_entity_data(std::vector<std::byte>(blob.begin(), blob.end()));
    if (!entity_mgr_.assign_dbid(ent->entity_id(), dbid))
    {
        if (auto* blocking_ent = entity_mgr_.find_by_dbid(dbid); blocking_ent)
        {
            entity_mgr_.destroy(ent->entity_id());
            defer_prepare_login_from_checkout(blocking_ent->entity_id(), std::move(pending), dbid,
                                              type_id, blob);
            start_disconnect_logoff(blocking_ent->entity_id());
            return;
        }

        entity_mgr_.destroy(ent->entity_id());
        release_checkout(dbid, type_id);
        reply.success = false;
        reply.error = "dbid_conflict";
        send_prepare_login_result(pending.loginapp_addr, reply);
        finish_login_flow(pending.dbid);
        return;
    }

    auto* proxy = entity_mgr_.find_proxy(ent->entity_id());
    if (proxy && !rotate_proxy_session(proxy->entity_id(), pending.session_key))
    {
        entity_mgr_.destroy(ent->entity_id());
        release_checkout(dbid, type_id);
        reply.success = false;
        reply.error = "session_conflict";
        send_prepare_login_result(pending.loginapp_addr, reply);
        finish_login_flow(pending.dbid);
        return;
    }

    if (!restore_managed_entity(ent->entity_id(), type_id, dbid, blob))
    {
        (void)notify_managed_entity_destroyed(ent->entity_id(), "login rollback");
        entity_mgr_.destroy(ent->entity_id());
        release_checkout(dbid, type_id);
        reply.success = false;
        reply.error = "restore_entity_failed";
        send_prepare_login_result(pending.loginapp_addr, reply);
        finish_login_flow(pending.dbid);
        return;
    }

    prepared_login_entities_[pending.login_request_id] =
        PreparedLoginEntity{ent->entity_id(), dbid, type_id, Clock::now()};
    prepared_login_requests_by_entity_[ent->entity_id()] = pending.login_request_id;

    reply.success = true;
    reply.entity_id = ent->entity_id();
    send_prepare_login_result(pending.loginapp_addr, reply);
    finish_login_flow(pending.dbid);

    ATLAS_LOG_DEBUG("BaseApp: login entity created id={} dbid={}", ent->entity_id(), dbid);
}

void BaseApp::defer_prepare_login_from_checkout(EntityID blocking_entity_id, PendingLogin pending,
                                                DatabaseID dbid, uint16_t type_id,
                                                std::span<const std::byte> blob)
{
    DeferredLoginCheckout deferred;
    deferred.pending = std::move(pending);
    deferred.dbid = dbid;
    deferred.type_id = type_id;
    deferred.blob.assign(blob.begin(), blob.end());
    deferred_login_checkouts_[blocking_entity_id].push_back(std::move(deferred));
}

void BaseApp::fail_deferred_prepare_logins(EntityID blocking_entity_id, std::string_view reason,
                                           std::vector<DatabaseID>* finished_dbids)
{
    auto it = deferred_login_checkouts_.find(blocking_entity_id);
    if (it == deferred_login_checkouts_.end())
    {
        return;
    }

    auto deferred = std::move(it->second);
    deferred_login_checkouts_.erase(it);
    std::vector<DatabaseID> local_finished_dbids;
    auto* completion_dbids = finished_dbids != nullptr ? finished_dbids : &local_finished_dbids;
    for (auto& entry : deferred)
    {
        release_checkout(entry.dbid, entry.type_id);
        fail_pending_prepare_login(entry.pending, reason);
        completion_dbids->push_back(entry.pending.dbid);
    }

    if (finished_dbids == nullptr)
    {
        drain_finished_login_flows(std::move(local_finished_dbids));
    }
}

auto BaseApp::resume_deferred_prepare_logins(EntityID blocking_entity_id) -> bool
{
    auto it = deferred_login_checkouts_.find(blocking_entity_id);
    if (it == deferred_login_checkouts_.end())
    {
        return false;
    }

    auto deferred = std::move(it->second);
    deferred_login_checkouts_.erase(it);
    for (auto& entry : deferred)
    {
        complete_prepare_login_from_checkout(std::move(entry.pending), entry.dbid, entry.type_id,
                                             entry.blob);
    }
    return !deferred.empty();
}

void BaseApp::submit_prepare_login(PendingLogin pending)
{
    if (!active_login_dbids_.insert(pending.dbid).second)
    {
        auto queued_it = queued_logins_.find(pending.dbid);
        if (queued_it != queued_logins_.end())
        {
            fail_pending_prepare_login(queued_it->second, "superseded_by_newer_login");
        }
        queued_logins_[pending.dbid] = std::move(pending);
        return;
    }

    dispatch_prepare_login(std::move(pending));
}

void BaseApp::dispatch_prepare_login(PendingLogin pending)
{
    if (try_complete_local_relogin(pending))
    {
        return;
    }

    const bool already_online = entity_mgr_.find_by_dbid(pending.dbid) != nullptr;
    if (already_online)
    {
        const uint32_t rid = next_prepare_request_id_++;
        pending_force_logoffs_[rid] = std::move(pending);

        baseapp::ForceLogoff fo;
        fo.dbid = pending_force_logoffs_[rid].dbid;
        fo.request_id = rid;
        process_force_logoff_request(fo);
        return;
    }

    // If LoginApp already checked out and provided the blob, skip DBApp round-trip
    if (pending.blob_prefetched)
    {
        const DatabaseID dbid = pending.dbid;
        const uint16_t type_id = pending.type_id;
        auto blob = std::move(pending.entity_blob);
        complete_prepare_login_from_checkout(std::move(pending), dbid, type_id,
                                             std::span<const std::byte>(blob));
        return;
    }

    const uint32_t rid = next_prepare_request_id_++;
    const DatabaseID dbid = pending.dbid;
    const uint16_t type_id = pending.type_id;
    pending_logins_[rid] = std::move(pending);

    if (!dbapp_channel_)
    {
        ATLAS_LOG_ERROR("BaseApp: PrepareLogin: no DBApp connection");
        fail_pending_prepare_login(rid, "no_dbapp");
        finish_login_flow(dbid);
        return;
    }

    dbapp::CheckoutEntity co;
    co.request_id = rid;
    co.dbid = dbid;
    co.type_id = type_id;
    auto send_result = dbapp_channel_->send_message(co);
    if (!send_result)
    {
        fail_pending_prepare_login(rid, "checkout_send_failed");
        finish_login_flow(dbid);
    }
}

void BaseApp::finish_login_flow(DatabaseID dbid)
{
    if (dbid == kInvalidDBID)
    {
        return;
    }

    active_login_dbids_.erase(dbid);

    auto queued_it = queued_logins_.find(dbid);
    if (queued_it == queued_logins_.end())
    {
        return;
    }

    PendingLogin next = std::move(queued_it->second);
    queued_logins_.erase(queued_it);

    active_login_dbids_.insert(dbid);
    dispatch_prepare_login(std::move(next));
}

void BaseApp::drain_finished_login_flows(std::vector<DatabaseID> dbids)
{
    dbids.erase(std::remove(dbids.begin(), dbids.end(), kInvalidDBID), dbids.end());
    if (dbids.empty())
    {
        return;
    }

    std::sort(dbids.begin(), dbids.end());
    dbids.erase(std::unique(dbids.begin(), dbids.end()), dbids.end());
    for (DatabaseID dbid : dbids)
    {
        finish_login_flow(dbid);
    }
}

void BaseApp::flush_remote_force_logoff_acks(EntityID entity_id, bool success)
{
    auto it = pending_remote_force_logoff_acks_.find(entity_id);
    if (it == pending_remote_force_logoff_acks_.end())
    {
        return;
    }

    for (const PendingRemoteForceLogoffAck& pending_ack : it->second)
    {
        if (auto* reply_ch = resolve_internal_channel(pending_ack.reply_addr))
        {
            baseapp::ForceLogoffAck ack;
            ack.request_id = pending_ack.request_id;
            ack.success = success;
            (void)reply_ch->send_message(ack);
        }
    }

    pending_remote_force_logoff_acks_.erase(it);
}

void BaseApp::flush_all_remote_force_logoff_acks(bool success)
{
    auto pending_acks = std::move(pending_remote_force_logoff_acks_);
    pending_remote_force_logoff_acks_.clear();

    for (const auto& [entity_id, queued] : pending_acks)
    {
        (void)entity_id;
        for (const PendingRemoteForceLogoffAck& pending_ack : queued)
        {
            if (auto* reply_ch = resolve_internal_channel(pending_ack.reply_addr))
            {
                baseapp::ForceLogoffAck ack;
                ack.request_id = pending_ack.request_id;
                ack.success = success;
                (void)reply_ch->send_message(ack);
            }
        }
    }
}

void BaseApp::begin_logoff_persist(EntityID entity_id, DatabaseID dbid, uint16_t type_id,
                                   uint32_t continuation_request_id)
{
    if (entity_id == kInvalidEntityID)
    {
        if (continuation_request_id != 0)
            continue_login_after_force_logoff(continuation_request_id);
        else
            finish_login_flow(dbid);
        return;
    }

    auto* ent = entity_mgr_.find(entity_id);
    if (!ent || ent->dbid() != dbid || ent->is_pending_destroy())
    {
        if (continuation_request_id != 0)
            continue_login_after_force_logoff(continuation_request_id);
        else
            finish_login_flow(dbid);
        return;
    }

    std::vector<std::byte> blob;
    if (!capture_entity_snapshot(entity_id, blob))
    {
        if (continuation_request_id != 0)
            fail_pending_force_logoff(continuation_request_id, "force_logoff_snapshot_failed");
        flush_remote_force_logoff_acks(entity_id, false);
        finish_login_flow(dbid);
        return;
    }

    // Optimistic checkin: release the DB-side checkout immediately so that a
    // subsequent CheckoutEntity for the same DBID can succeed without waiting
    // for the data write to complete.
    release_checkout(dbid, type_id);

    // Fire-and-forget data persistence — the checkout is already released so
    // we don't use WriteFlags::LogOff.  The WriteEntityAck for this request
    // will be silently ignored (entity already destroyed locally).
    if (dbapp_channel_)
    {
        dbapp::WriteEntity msg;
        msg.flags = WriteFlags::ExplicitDBID;
        msg.type_id = type_id;
        msg.dbid = dbid;
        msg.entity_id = entity_id;
        msg.request_id = next_prepare_request_id_++;
        msg.blob = std::move(blob);
        (void)dbapp_channel_->send_message(msg);
    }

    // Destroy the local entity immediately.
    if (!finalize_force_logoff(entity_id))
    {
        if (continuation_request_id != 0)
            fail_pending_force_logoff(continuation_request_id, "force_logoff_destroy_failed");
        if (auto waiter_it = pending_local_force_logoff_waiters_.find(entity_id);
            waiter_it != pending_local_force_logoff_waiters_.end())
        {
            for (uint32_t waiter_request_id : waiter_it->second)
                fail_pending_force_logoff(waiter_request_id, "force_logoff_destroy_failed");
            pending_local_force_logoff_waiters_.erase(waiter_it);
        }
        fail_deferred_prepare_logins(entity_id, "force_logoff_destroy_failed");
        flush_remote_force_logoff_acks(entity_id, false);
        finish_login_flow(dbid);
        return;
    }

    // All post-logoff continuations are processed synchronously — no need to
    // wait for WriteEntityAck.
    const bool resumed_deferred = resume_deferred_prepare_logins(entity_id);

    if (auto waiter_it = pending_local_force_logoff_waiters_.find(entity_id);
        waiter_it != pending_local_force_logoff_waiters_.end())
    {
        for (uint32_t waiter_request_id : waiter_it->second)
        {
            if (pending_force_logoffs_.contains(waiter_request_id))
                continue_login_after_force_logoff(waiter_request_id);
        }
        pending_local_force_logoff_waiters_.erase(waiter_it);
    }

    flush_remote_force_logoff_acks(entity_id, true);

    if (continuation_request_id != 0 && pending_force_logoffs_.contains(continuation_request_id))
    {
        continue_login_after_force_logoff(continuation_request_id);
    }
    else if (!resumed_deferred)
    {
        finish_login_flow(dbid);
    }
}

void BaseApp::begin_force_logoff_persist(uint32_t force_request_id, EntityID entity_id)
{
    auto* ent = entity_mgr_.find(entity_id);
    if (!ent || ent->dbid() == kInvalidDBID)
    {
        if (!finalize_force_logoff(entity_id))
        {
            fail_pending_force_logoff(force_request_id, "force_logoff_destroy_failed");
            auto pending_it = pending_force_logoffs_.find(force_request_id);
            if (pending_it != pending_force_logoffs_.end())
            {
                finish_login_flow(pending_it->second.dbid);
            }
            return;
        }
        continue_login_after_force_logoff(force_request_id);
        return;
    }

    begin_logoff_persist(entity_id, ent->dbid(), ent->type_id(), force_request_id);
}

void BaseApp::start_disconnect_logoff(EntityID entity_id)
{
    clear_detached_grace(entity_id);

    auto* ent = entity_mgr_.find(entity_id);
    if (!ent || ent->is_pending_destroy())
    {
        return;
    }

    const DatabaseID dbid = ent->dbid();
    if (dbid != kInvalidDBID)
    {
        active_login_dbids_.insert(dbid);
    }

    if (dbid == kInvalidDBID)
    {
        (void)finalize_force_logoff(entity_id);
        return;
    }

    begin_logoff_persist(entity_id, dbid, ent->type_id(), 0);
}

void BaseApp::continue_login_after_force_logoff(uint32_t force_request_id)
{
    auto it = pending_force_logoffs_.find(force_request_id);
    if (it == pending_force_logoffs_.end())
    {
        return;
    }

    PendingLogin pending = it->second;
    pending_force_logoffs_.erase(it);

    if (pending.reply_sent)
    {
        return;
    }

    if (!dbapp_channel_)
    {
        fail_pending_prepare_login(pending, "no_dbapp");
        finish_login_flow(pending.dbid);
        return;
    }

    uint32_t new_rid = next_prepare_request_id_++;
    pending_logins_[new_rid] = pending;

    dbapp::CheckoutEntity co;
    co.request_id = new_rid;
    co.dbid = pending.dbid;
    co.type_id = pending.type_id;
    auto send_result = dbapp_channel_->send_message(co);
    if (!send_result)
    {
        pending_logins_.erase(new_rid);
        fail_pending_prepare_login(pending, "checkout_send_failed");
        finish_login_flow(pending.dbid);
    }
}

auto BaseApp::finalize_force_logoff(EntityID entity_id) -> bool
{
    auto* ent = entity_mgr_.find(entity_id);
    if (!ent)
    {
        return true;
    }

    if (!notify_managed_entity_destroyed(entity_id, "force logoff"))
    {
        return false;
    }

    clear_detached_grace(entity_id);
    unbind_client(entity_id);
    (void)entity_mgr_.clear_session_key(entity_id);
    (void)entity_mgr_.assign_dbid(entity_id, kInvalidDBID);
    ent->mark_for_destroy();
    return true;
}

void BaseApp::process_force_logoff_request(const baseapp::ForceLogoff& msg)
{
    EntityID found_id = kInvalidEntityID;
    if (auto* ent = entity_mgr_.find_by_dbid(msg.dbid))
    {
        found_id = ent->entity_id();
    }

    if (found_id != kInvalidEntityID)
    {
        if (auto* found_ent = entity_mgr_.find(found_id);
            found_ent && found_ent->is_pending_destroy())
        {
            if (pending_force_logoffs_.contains(msg.request_id))
                continue_login_after_force_logoff(msg.request_id);
            return;
        }

        ++force_logoff_total_;
        ATLAS_LOG_DEBUG("BaseApp: ForceLogoff: processing entity={} dbid={}", found_id, msg.dbid);
        if (pending_force_logoffs_.contains(msg.request_id))
        {
            begin_force_logoff_persist(msg.request_id, found_id);
            return;
        }

        (void)finalize_force_logoff(found_id);
    }

    if (pending_force_logoffs_.contains(msg.request_id))
    {
        continue_login_after_force_logoff(msg.request_id);
    }
}

void BaseApp::do_give_client_to_local(EntityID src_id, EntityID dest_id)
{
    auto* src_proxy = entity_mgr_.find_proxy(src_id);
    auto* dst_proxy = entity_mgr_.find_proxy(dest_id);
    if (!src_proxy || !dst_proxy)
    {
        ATLAS_LOG_ERROR("BaseApp: give_client_to: invalid src={} or dest={}", src_id, dest_id);
        return;
    }
    if (!src_proxy->has_client())
    {
        ATLAS_LOG_WARNING("BaseApp: give_client_to: src={} has no client", src_id);
        return;
    }
    const auto session_key = src_proxy->session_key();
    (void)entity_mgr_.clear_session_key(src_id);
    if (!entity_mgr_.assign_session_key(dest_id, session_key))
    {
        ATLAS_LOG_ERROR("BaseApp: give_client_to: failed to move session key to dest={}", dest_id);
        return;
    }

    const auto client_addr = src_proxy->client_addr();
    unbind_client(src_id);
    if (!bind_client(dest_id, client_addr))
    {
        ATLAS_LOG_ERROR("BaseApp: give_client_to: failed to move client binding to dest={}",
                        dest_id);
    }
}

void BaseApp::do_give_client_to_remote(EntityID src_id, EntityID /*dest_id*/,
                                       const Address& dest_baseapp)
{
    auto* src_proxy = entity_mgr_.find_proxy(src_id);
    if (!src_proxy || !src_proxy->has_client())
    {
        ATLAS_LOG_ERROR("BaseApp: give_client_to_remote: src={} has no client", src_id);
        return;
    }
    // Send AcceptClient to the target BaseApp (its internal RUDP address)
    auto dest_ch_result = network().connect_rudp_nocwnd(dest_baseapp);
    if (!dest_ch_result)
    {
        ATLAS_LOG_ERROR("BaseApp: give_client_to_remote: could not connect to {}:{}",
                        dest_baseapp.ip(), dest_baseapp.port());
        return;
    }

    baseapp::AcceptClient accept;
    accept.dest_entity_id = src_id;
    accept.session_key = src_proxy->session_key();
    (void)(*dest_ch_result)->send_message(accept);

    unbind_client(src_id);
}

// ============================================================================
// Login flow handlers
// ============================================================================

void BaseApp::on_register_baseapp_ack(Channel& /*ch*/, const baseappmgr::RegisterBaseAppAck& msg)
{
    if (!msg.success)
    {
        ATLAS_LOG_ERROR("BaseApp: RegisterBaseAppAck failed — shutting down");
        shutdown();
        return;
    }
    app_id_ = msg.app_id;
    entity_mgr_.set_id_range(msg.entity_id_start, msg.entity_id_end);

    // Notify BaseAppMgr that we are ready
    if (baseappmgr_channel_)
    {
        baseappmgr::BaseAppReady ready;
        ready.app_id = app_id_;
        (void)baseappmgr_channel_->send_message(ready);
    }

    ATLAS_LOG_INFO("BaseApp: registered as app_id={} id_range=[{},{}]", app_id_,
                   msg.entity_id_start, msg.entity_id_end);
}

void BaseApp::on_request_entity_id_range_ack(Channel& /*ch*/,
                                             const baseappmgr::RequestEntityIdRangeAck& msg)
{
    entity_mgr_.extend_id_range(msg.entity_id_end);
    id_range_requested_ = false;
    ATLAS_LOG_INFO("BaseApp: EntityID range extended to [{},{}]", msg.entity_id_start,
                   msg.entity_id_end);
}

auto BaseApp::resolve_internal_channel(const Address& addr) -> Channel*
{
    if (auto* existing = network().find_channel(addr))
    {
        return existing;
    }

    auto result = network().connect_rudp_nocwnd(addr);
    if (!result)
    {
        ATLAS_LOG_WARNING("BaseApp: failed to resolve internal channel {}:{}", addr.ip(),
                          addr.port());
        return nullptr;
    }

    return static_cast<Channel*>(*result);
}

auto BaseApp::resolve_client_channel(EntityID entity_id) -> Channel*
{
    auto it = entity_client_index_.find(entity_id);
    if (it == entity_client_index_.end())
    {
        return nullptr;
    }

    auto* proxy = entity_mgr_.find_proxy(entity_id);
    if (!proxy || !proxy->has_client())
    {
        auto reverse = client_entity_index_.find(it->second);
        if (reverse != client_entity_index_.end() && reverse->second == entity_id)
        {
            client_entity_index_.erase(reverse);
        }
        entity_client_index_.erase(it);
        return nullptr;
    }

    auto* ch = external_network_.find_channel(it->second);
    if (!ch)
    {
        unbind_client(entity_id);
        return nullptr;
    }

    return ch;
}

auto BaseApp::bind_client(EntityID entity_id, const Address& client_addr) -> bool
{
    auto* proxy = entity_mgr_.find_proxy(entity_id);
    if (!proxy)
    {
        return false;
    }

    auto existing_client = client_entity_index_.find(client_addr);
    if (existing_client != client_entity_index_.end() && existing_client->second != entity_id)
    {
        unbind_client(existing_client->second);
    }

    auto existing_entity = entity_client_index_.find(entity_id);
    if (existing_entity != entity_client_index_.end() && existing_entity->second != client_addr)
    {
        client_entity_index_.erase(existing_entity->second);
    }

    clear_detached_grace(entity_id);
    entity_client_index_[entity_id] = client_addr;
    client_entity_index_[client_addr] = entity_id;
    proxy->bind_client(client_addr);
    return true;
}

void BaseApp::unbind_client(EntityID entity_id)
{
    auto it = entity_client_index_.find(entity_id);
    if (it != entity_client_index_.end())
    {
        auto reverse = client_entity_index_.find(it->second);
        if (reverse != client_entity_index_.end() && reverse->second == entity_id)
        {
            client_entity_index_.erase(reverse);
        }
        entity_client_index_.erase(it);
    }

    if (auto* proxy = entity_mgr_.find_proxy(entity_id))
    {
        proxy->unbind_client();
    }
}

void BaseApp::on_external_client_disconnect(Channel& ch)
{
    auto it = client_entity_index_.find(ch.remote_address());
    if (it == client_entity_index_.end())
    {
        return;
    }

    const auto entity_id = it->second;
    unbind_client(entity_id);
    if (auto* proxy = entity_mgr_.find_proxy(entity_id); proxy && proxy->dbid() != kInvalidDBID)
    {
        enter_detached_grace(entity_id);
        ATLAS_LOG_DEBUG("BaseApp: client disconnected from entity={} entering detached grace",
                        entity_id);
        return;
    }

    start_disconnect_logoff(entity_id);
    ATLAS_LOG_DEBUG("BaseApp: client disconnected from entity={}", entity_id);
}

void BaseApp::send_prepare_login_result(const Address& reply_addr,
                                        const login::PrepareLoginResult& msg)
{
    if (auto* ch = this->resolve_internal_channel(reply_addr))
    {
        (void)ch->send_message(msg);
    }
}

void BaseApp::cleanup_expired_pending_requests()
{
    const auto now = Clock::now();
    struct ExpiredLoginRequest
    {
        uint32_t request_id{0};
        PendingLogin pending;
    };
    std::vector<ExpiredLoginRequest> expired_login_requests;
    std::vector<uint32_t> expired_force_logoff_requests;
    std::vector<uint32_t> stalled_force_logoffs;
    std::vector<uint32_t> expired_prepared_login_requests;
    std::vector<DatabaseID> finished_login_dbids;
    std::unordered_set<uint32_t> expired_force_logoff_request_ids;

    std::erase_if(canceled_login_checkouts_, [now](const auto& entry)
                  { return now - entry.second.canceled_at > kCanceledCheckoutRetention; });

    for (auto& [request_id, pending] : pending_logins_)
    {
        if (!pending.reply_sent && now - pending.created_at > kPendingTimeout)
        {
            ATLAS_LOG_WARNING("BaseApp: prepare-login request_id={} timed out", request_id);
            fail_pending_prepare_login(pending, "timeout");
            finished_login_dbids.push_back(pending.dbid);
            expired_login_requests.push_back(ExpiredLoginRequest{request_id, pending});
        }
    }

    for (auto& [request_id, pending] : pending_force_logoffs_)
    {
        if (!pending.reply_sent && now - pending.created_at > kPendingTimeout)
        {
            ATLAS_LOG_WARNING("BaseApp: force-logoff request_id={} timed out", request_id);
            fail_pending_force_logoff(pending, "timeout");
            finished_login_dbids.push_back(pending.dbid);
            expired_force_logoff_requests.push_back(request_id);
            expired_force_logoff_request_ids.insert(request_id);
        }
        else if (pending.next_force_logoff_retry_at != TimePoint{} &&
                 now >= pending.next_force_logoff_retry_at)
        {
            stalled_force_logoffs.push_back(request_id);
        }
    }

    for (auto it = pending_logoff_writes_.begin(); it != pending_logoff_writes_.end();)
    {
        const bool timed_out = now - it->second.created_at > kPendingTimeout;
        const bool orphaned =
            it->second.continuation_request_id != 0 &&
            (!pending_force_logoffs_.contains(it->second.continuation_request_id) ||
             expired_force_logoff_request_ids.contains(it->second.continuation_request_id));
        if (timed_out || orphaned)
        {
            if (timed_out)
            {
                ATLAS_LOG_WARNING("BaseApp: logoff write request_id={} entity_id={} timed out",
                                  it->first, it->second.entity_id);
            }

            logoff_entities_in_flight_.erase(it->second.entity_id);
            fail_deferred_prepare_logins(it->second.entity_id, "timeout", &finished_login_dbids);
            if (auto waiter_it = pending_local_force_logoff_waiters_.find(it->second.entity_id);
                waiter_it != pending_local_force_logoff_waiters_.end())
            {
                for (uint32_t waiter_request_id : waiter_it->second)
                {
                    fail_pending_force_logoff(waiter_request_id, "timeout");
                }
                pending_local_force_logoff_waiters_.erase(waiter_it);
            }
            flush_remote_force_logoff_acks(it->second.entity_id, false);
            finished_login_dbids.push_back(it->second.dbid);
            it = pending_logoff_writes_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = deferred_login_checkouts_.begin(); it != deferred_login_checkouts_.end();)
    {
        auto& deferred = it->second;
        for (auto entry_it = deferred.begin(); entry_it != deferred.end();)
        {
            if (!entry_it->pending.reply_sent &&
                now - entry_it->pending.created_at > kPendingTimeout)
            {
                release_checkout(entry_it->dbid, entry_it->type_id);
                fail_pending_prepare_login(entry_it->pending, "timeout");
                finished_login_dbids.push_back(entry_it->pending.dbid);
                entry_it = deferred.erase(entry_it);
            }
            else
            {
                ++entry_it;
            }
        }

        if (deferred.empty())
        {
            it = deferred_login_checkouts_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (auto it = queued_logins_.begin(); it != queued_logins_.end();)
    {
        if (!it->second.reply_sent && now - it->second.created_at > kPendingTimeout)
        {
            ATLAS_LOG_WARNING("BaseApp: queued prepare-login dbid={} timed out", it->first);
            fail_pending_prepare_login(it->second, "timeout");
            it = queued_logins_.erase(it);
        }
        else
        {
            ++it;
        }
    }

    for (const auto& expired : expired_login_requests)
    {
        cancel_inflight_checkout(expired.request_id, expired.pending);
        pending_logins_.erase(expired.request_id);
    }

    for (uint32_t request_id : expired_force_logoff_requests)
    {
        pending_force_logoffs_.erase(request_id);
    }

    for (uint32_t request_id : stalled_force_logoffs)
    {
        retry_stalled_force_logoff(request_id);
    }

    for (const auto& [login_request_id, prepared] : prepared_login_entities_)
    {
        if (now - prepared.prepared_at > kPreparedLoginTimeout)
        {
            expired_prepared_login_requests.push_back(login_request_id);
        }
    }

    for (uint32_t login_request_id : expired_prepared_login_requests)
    {
        ATLAS_LOG_WARNING(
            "BaseApp: prepared login request_id={} expired before client "
            "authenticate",
            login_request_id);
        (void)rollback_prepared_login_entity(login_request_id);
    }

    drain_finished_login_flows(std::move(finished_login_dbids));
}

void BaseApp::fail_all_dbapp_pending_requests(std::string_view reason)
{
    for (auto& [request_id, pending] : pending_logins_)
    {
        (void)request_id;
        fail_pending_prepare_login(pending, reason);
    }
    pending_logins_.clear();

    for (auto& [request_id, pending] : pending_force_logoffs_)
    {
        (void)request_id;
        fail_pending_force_logoff(pending, reason);
    }
    pending_force_logoffs_.clear();
    pending_logoff_writes_.clear();
    canceled_login_checkouts_.clear();
    flush_all_remote_force_logoff_acks(false);
    for (auto& [entity_id, deferred] : deferred_login_checkouts_)
    {
        (void)entity_id;
        for (auto& entry : deferred)
        {
            fail_pending_prepare_login(entry.pending, reason);
        }
    }
    deferred_login_checkouts_.clear();
    pending_local_force_logoff_waiters_.clear();
    logoff_entities_in_flight_.clear();

    for (auto& [dbid, pending] : queued_logins_)
    {
        (void)dbid;
        fail_pending_prepare_login(pending, reason);
    }
    queued_logins_.clear();
    active_login_dbids_.clear();
}

void BaseApp::fail_pending_prepare_login(PendingLogin& pending, std::string_view reason)
{
    if (pending.reply_sent)
    {
        return;
    }

    login::PrepareLoginResult reply;
    reply.request_id = pending.login_request_id;
    reply.success = false;
    reply.error = std::string(reason);
    send_prepare_login_result(pending.loginapp_addr, reply);
    pending.reply_sent = true;
}

void BaseApp::fail_pending_prepare_login(uint32_t request_id, std::string_view reason)
{
    auto it = pending_logins_.find(request_id);
    if (it == pending_logins_.end())
    {
        return;
    }
    fail_pending_prepare_login(it->second, reason);
    pending_logins_.erase(it);
}

void BaseApp::fail_pending_force_logoff(PendingLogin& pending, std::string_view reason)
{
    if (pending.reply_sent)
    {
        return;
    }

    login::PrepareLoginResult reply;
    reply.request_id = pending.login_request_id;
    reply.success = false;
    reply.error = std::string(reason);
    send_prepare_login_result(pending.loginapp_addr, reply);
    pending.reply_sent = true;
}

void BaseApp::fail_pending_force_logoff(uint32_t request_id, std::string_view reason)
{
    auto it = pending_force_logoffs_.find(request_id);
    if (it == pending_force_logoffs_.end())
    {
        return;
    }
    fail_pending_force_logoff(it->second, reason);
    pending_force_logoffs_.erase(it);
}

void BaseApp::schedule_force_logoff_retry(PendingLogin& pending, TimePoint now)
{
    const auto shift = std::min<uint8_t>(pending.force_logoff_retry_count, 3);
    const auto delay =
        std::min(kForceLogoffRetryBaseDelay * (1 << shift), kForceLogoffRetryMaxDelay);
    pending.next_force_logoff_retry_at = now + delay;
    if (pending.force_logoff_retry_count < std::numeric_limits<uint8_t>::max())
    {
        ++pending.force_logoff_retry_count;
    }
}

void BaseApp::retry_stalled_force_logoff(uint32_t request_id)
{
    auto it = pending_force_logoffs_.find(request_id);
    if (it == pending_force_logoffs_.end())
    {
        return;
    }

    PendingLogin& pending = it->second;
    if (!pending.waiting_for_remote_force_logoff_ack ||
        pending.force_logoff_holder_addr.port() == 0 ||
        pending.force_logoff_holder_addr == network().rudp_address())
    {
        pending.next_force_logoff_retry_at = {};
        pending.waiting_for_remote_force_logoff_ack = false;
        return;
    }

    schedule_force_logoff_retry(pending, Clock::now());
    if (auto* holder_ch = resolve_internal_channel(pending.force_logoff_holder_addr))
    {
        baseapp::ForceLogoff force_logoff;
        force_logoff.dbid = pending.dbid;
        force_logoff.request_id = request_id;
        (void)holder_ch->send_message(force_logoff);
    }
}

void BaseApp::release_checkout(DatabaseID dbid, uint16_t type_id)
{
    if (!dbapp_channel_ || dbid == kInvalidDBID)
    {
        return;
    }

    dbapp::CheckinEntity checkin;
    checkin.type_id = type_id;
    checkin.dbid = dbid;
    (void)dbapp_channel_->send_message(checkin);
}

void BaseApp::cancel_inflight_checkout(uint32_t request_id, const PendingLogin& pending)
{
    canceled_login_checkouts_[request_id] =
        CanceledCheckout{pending.dbid, pending.type_id, Clock::now()};
    send_abort_checkout(request_id, pending.dbid, pending.type_id);
}

void BaseApp::send_abort_checkout(uint32_t request_id, DatabaseID dbid, uint16_t type_id)
{
    if (!dbapp_channel_)
    {
        ATLAS_LOG_WARNING(
            "BaseApp: cannot abort checkout request_id={} dbid={} without DBApp "
            "channel",
            request_id, dbid);
        return;
    }

    dbapp::AbortCheckout abort;
    abort.request_id = request_id;
    abort.type_id = type_id;
    abort.dbid = dbid;
    (void)dbapp_channel_->send_message(abort);
}

auto BaseApp::rollback_prepared_login_entity(uint32_t login_request_id) -> bool
{
    auto it = prepared_login_entities_.find(login_request_id);
    if (it == prepared_login_entities_.end())
    {
        return false;
    }

    const PreparedLoginEntity prepared = it->second;
    prepared_login_entities_.erase(it);
    prepared_login_requests_by_entity_.erase(prepared.entity_id);

    auto* ent = entity_mgr_.find(prepared.entity_id);
    if (!ent)
    {
        return true;
    }

    (void)notify_managed_entity_destroyed(prepared.entity_id, "prepare login cancel");
    clear_detached_grace(prepared.entity_id);
    unbind_client(prepared.entity_id);
    (void)entity_mgr_.clear_session_key(prepared.entity_id);
    (void)entity_mgr_.assign_dbid(prepared.entity_id, kInvalidDBID);
    ent->mark_for_destroy();
    release_checkout(prepared.dbid, prepared.type_id);
    return true;
}

void BaseApp::clear_prepared_login_entity(EntityID entity_id)
{
    auto it = prepared_login_requests_by_entity_.find(entity_id);
    if (it == prepared_login_requests_by_entity_.end())
    {
        return;
    }

    prepared_login_entities_.erase(it->second);
    prepared_login_requests_by_entity_.erase(it);
}

void BaseApp::cancel_prepare_login(uint32_t login_request_id, DatabaseID dbid)
{
    for (auto it = pending_logins_.begin(); it != pending_logins_.end(); ++it)
    {
        if (it->second.login_request_id != login_request_id)
        {
            continue;
        }

        cancel_inflight_checkout(it->first, it->second);
        finish_login_flow(it->second.dbid);
        pending_logins_.erase(it);
        return;
    }

    for (auto it = pending_force_logoffs_.begin(); it != pending_force_logoffs_.end(); ++it)
    {
        if (it->second.login_request_id != login_request_id)
        {
            continue;
        }

        finish_login_flow(it->second.dbid);
        pending_force_logoffs_.erase(it);
        return;
    }

    for (auto it = queued_logins_.begin(); it != queued_logins_.end(); ++it)
    {
        if (it->second.login_request_id != login_request_id)
        {
            continue;
        }

        queued_logins_.erase(it);
        return;
    }

    for (auto deferred_it = deferred_login_checkouts_.begin();
         deferred_it != deferred_login_checkouts_.end();)
    {
        auto& deferred = deferred_it->second;
        bool removed = false;
        for (auto entry_it = deferred.begin(); entry_it != deferred.end();)
        {
            if (entry_it->pending.login_request_id != login_request_id)
            {
                ++entry_it;
                continue;
            }

            release_checkout(entry_it->dbid, entry_it->type_id);
            finish_login_flow(entry_it->pending.dbid);
            entry_it = deferred.erase(entry_it);
            removed = true;
        }

        if (deferred.empty())
        {
            deferred_it = deferred_login_checkouts_.erase(deferred_it);
        }
        else
        {
            ++deferred_it;
        }

        if (removed)
        {
            return;
        }
    }

    if (rollback_prepared_login_entity(login_request_id))
    {
        return;
    }

    ATLAS_LOG_DEBUG("BaseApp: cancel prepare login request_id={} dbid={} ignored (not pending)",
                    login_request_id, dbid);
}

auto BaseApp::retry_login_after_checkout_conflict(PendingLogin pending, DatabaseID dbid,
                                                  const Address& holder_addr) -> bool
{
    if (holder_addr.port() == 0)
    {
        return false;
    }

    const uint32_t force_request_id = next_prepare_request_id_++;
    pending.force_logoff_holder_addr = holder_addr;
    pending.next_force_logoff_retry_at = {};
    pending.force_logoff_retry_count = 0;
    pending.waiting_for_remote_force_logoff_ack = false;
    pending_force_logoffs_[force_request_id] = std::move(pending);

    baseapp::ForceLogoff force_logoff;
    force_logoff.dbid = dbid;
    force_logoff.request_id = force_request_id;

    if (holder_addr == network().rudp_address())
    {
        process_force_logoff_request(force_logoff);
        return true;
    }

    auto pending_it = pending_force_logoffs_.find(force_request_id);
    if (pending_it == pending_force_logoffs_.end())
    {
        return true;
    }

    pending_it->second.waiting_for_remote_force_logoff_ack = true;
    schedule_force_logoff_retry(pending_it->second, Clock::now());

    if (auto* holder_ch = resolve_internal_channel(holder_addr))
    {
        const auto send_result = holder_ch->send_message(force_logoff);
        if (!send_result)
        {
            ATLAS_LOG_WARNING(
                "BaseApp: failed to send ForceLogoff dbid={} request_id={} to holder {}:{}: {}",
                dbid, force_request_id, holder_addr.ip(), holder_addr.port(),
                send_result.error().message());
        }
    }

    return true;
}

auto BaseApp::restore_managed_entity(EntityID entity_id, uint16_t type_id, DatabaseID dbid,
                                     std::span<const std::byte> blob) -> bool
{
    if (!has_managed_entity_type(type_id))
    {
        return true;
    }

    if (!native_provider_ || !native_provider_->restore_entity_fn())
    {
        return true;
    }

    clear_native_api_error();
    native_provider_->restore_entity_fn()(entity_id, type_id, dbid,
                                          reinterpret_cast<const uint8_t*>(blob.data()),
                                          static_cast<int32_t>(blob.size()));
    if (auto error = consume_native_api_error())
    {
        ATLAS_LOG_ERROR("BaseApp: restore_entity failed for entity={} type={} dbid={}: {}",
                        entity_id, type_id, dbid, *error);
        return false;
    }

    return true;
}

auto BaseApp::notify_managed_entity_destroyed(EntityID entity_id, std::string_view context) -> bool
{
    auto* ent = entity_mgr_.find(entity_id);
    if (!ent || !has_managed_entity_type(ent->type_id()))
    {
        return true;
    }

    if (!native_provider_ || !native_provider_->entity_destroyed_fn())
    {
        return true;
    }

    clear_native_api_error();
    native_provider_->entity_destroyed_fn()(entity_id);
    if (auto error = consume_native_api_error())
    {
        ATLAS_LOG_ERROR("BaseApp: entity_destroyed callback failed for entity={} during {}: {}",
                        entity_id, context, *error);
        return false;
    }

    return true;
}

auto BaseApp::capture_load_snapshot() const -> LoadSnapshot
{
    LoadSnapshot snapshot;
    snapshot.entity_count = static_cast<uint32_t>(entity_mgr_.size());
    snapshot.proxy_count = static_cast<uint32_t>(entity_mgr_.proxy_count());
    snapshot.pending_prepare_count = static_cast<uint32_t>(pending_logins_.size());
    snapshot.pending_force_logoff_count = static_cast<uint32_t>(pending_force_logoffs_.size());
    snapshot.detached_proxy_count = static_cast<uint32_t>(detached_proxies_.size());
    snapshot.logoff_in_flight_count = static_cast<uint32_t>(logoff_entities_in_flight_.size());
    snapshot.deferred_login_count = static_cast<uint32_t>(deferred_login_checkout_count());
    return snapshot;
}

void BaseApp::update_load_estimate()
{
    load_tracker_.observe_tick_complete(config().update_hertz, capture_load_snapshot());
}

void BaseApp::report_load_to_baseappmgr()
{
    if (!baseappmgr_channel_ || app_id_ == 0)
    {
        return;
    }

    const auto msg = load_tracker_.build_report(app_id_, capture_load_snapshot());
    (void)baseappmgr_channel_->send_message(msg);
}

void BaseApp::maybe_request_more_ids()
{
    if (!id_range_requested_ && baseappmgr_channel_ && entity_mgr_.is_range_low())
    {
        baseappmgr::RequestEntityIdRange req;
        req.app_id = app_id_;
        (void)baseappmgr_channel_->send_message(req);
        id_range_requested_ = true;
        ATLAS_LOG_INFO("BaseApp: requested more EntityIDs from BaseAppMgr");
    }
}

void BaseApp::on_prepare_login(Channel& ch, const login::PrepareLogin& msg)
{
    ATLAS_LOG_DEBUG("BaseApp: prepare login request_id={} dbid={} type_id={} blob={}B from {}:{}",
                    msg.request_id, msg.dbid, msg.type_id, msg.entity_blob.size(),
                    ch.remote_address().ip(), ch.remote_address().port());
    PendingLogin pending;
    pending.login_request_id = msg.request_id;
    pending.loginapp_addr = ch.remote_address();
    pending.type_id = msg.type_id;
    pending.dbid = msg.dbid;
    pending.session_key = msg.session_key;
    pending.created_at = Clock::now();
    pending.blob_prefetched = msg.blob_prefetched;
    pending.entity_blob = msg.entity_blob;
    submit_prepare_login(std::move(pending));
    (void)msg.client_addr;
}

void BaseApp::on_cancel_prepare_login(Channel& /*ch*/, const login::CancelPrepareLogin& msg)
{
    cancel_prepare_login(msg.request_id, msg.dbid);
}

void BaseApp::on_force_logoff(Channel& ch, const baseapp::ForceLogoff& msg)
{
    if (pending_force_logoffs_.contains(msg.request_id))
    {
        process_force_logoff_request(msg);
        return;
    }

    EntityID found_id = kInvalidEntityID;
    if (auto* ent = entity_mgr_.find_by_dbid(msg.dbid))
    {
        found_id = ent->entity_id();
    }

    if (found_id == kInvalidEntityID)
    {
        baseapp::ForceLogoffAck ack;
        ack.request_id = msg.request_id;
        ack.success = true;
        (void)ch.send_message(ack);
        return;
    }

    pending_remote_force_logoff_acks_[found_id].push_back({ch.remote_address(), msg.request_id});

    auto* ent = entity_mgr_.find(found_id);
    if (!ent || ent->is_pending_destroy())
    {
        flush_remote_force_logoff_acks(found_id, true);
        return;
    }

    if (ent->dbid() == kInvalidDBID)
    {
        (void)finalize_force_logoff(found_id);
        flush_remote_force_logoff_acks(found_id, true);
        return;
    }

    ++force_logoff_total_;
    begin_logoff_persist(found_id, ent->dbid(), ent->type_id(), 0);
}

void BaseApp::on_force_logoff_ack(Channel& /*ch*/, const baseapp::ForceLogoffAck& msg)
{
    auto it = pending_force_logoffs_.find(msg.request_id);
    if (it == pending_force_logoffs_.end())
        return;

    if (!msg.success)
    {
        PendingLogin& pending = it->second;
        login::PrepareLoginResult reply;
        reply.request_id = pending.login_request_id;
        reply.success = false;
        reply.error = "force_logoff_failed";
        send_prepare_login_result(pending.loginapp_addr, reply);
        finish_login_flow(pending.dbid);
        pending_force_logoffs_.erase(it);
        return;
    }

    // Logoff succeeded — now checkout entity from DBApp
    it->second.waiting_for_remote_force_logoff_ack = false;
    it->second.next_force_logoff_retry_at = {};
    continue_login_after_force_logoff(msg.request_id);
}

// ============================================================================
// on_client_authenticate — handle first message from a client
// ============================================================================

void BaseApp::on_client_authenticate(Channel& ch, const baseapp::Authenticate& msg)
{
    auto* proxy = entity_mgr_.find_proxy_by_session(msg.session_key);
    if (!proxy)
    {
        ++auth_fail_total_;
        ATLAS_LOG_WARNING("BaseApp: Authenticate: no matching session key");
        baseapp::AuthenticateResult res;
        res.success = false;
        res.error = "invalid_session";
        (void)ch.send_message(res);
        return;
    }

    if (!bind_client(proxy->entity_id(), ch.remote_address()))
    {
        ++auth_fail_total_;
        baseapp::AuthenticateResult res;
        res.success = false;
        res.error = "bind_client_failed";
        (void)ch.send_message(res);
        return;
    }

    baseapp::AuthenticateResult res;
    res.success = true;
    res.entity_id = proxy->entity_id();
    res.type_id = proxy->type_id();
    (void)ch.send_message(res);
    ++auth_success_total_;
    clear_prepared_login_entity(proxy->entity_id());

    ATLAS_LOG_DEBUG("BaseApp: client authenticated as entity={}", proxy->entity_id());
}

}  // namespace atlas
