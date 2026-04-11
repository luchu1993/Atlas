#include "baseapp.hpp"

#include "baseapp_messages.hpp"
#include "baseapp_native_provider.hpp"
#include "baseappmgr/baseappmgr_messages.hpp"
#include "db/idatabase.hpp"
#include "dbapp/dbapp_messages.hpp"
#include "foundation/log.hpp"
#include "loginapp/login_messages.hpp"
#include "network/channel.hpp"
#include "network/machined_types.hpp"
#include "network/reliable_udp.hpp"
#include "script/script_value.hpp"
#include "server/watcher.hpp"

#include <chrono>
#include <format>
#include <span>
#include <vector>

namespace atlas
{

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
        auto listen_result = external_network_.start_rudp_server(ext_addr);
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
                auto ch = network().connect_rudp(n.internal_addr);
                if (ch)
                    dbapp_channel_ = static_cast<Channel*>(*ch);
            }
        },
        [this](const machined::DeathNotification& n)
        {
            (void)n;
            ATLAS_LOG_WARNING("BaseApp: DBApp died, clearing dbapp channel");
            dbapp_channel_ = nullptr;
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
                auto ch = network().connect_rudp(n.internal_addr);
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
    entity_mgr_.flush_destroyed();
    EntityApp::fini();
}

// ============================================================================
// on_tick_complete
// ============================================================================

void BaseApp::on_tick_complete()
{
    entity_mgr_.flush_destroyed();
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
    wr.add<std::size_t>("baseapp/entity_count",
                        std::function<std::size_t()>([this] { return entity_mgr_.size(); }));
    wr.add<std::size_t>(
        "baseapp/client_binding_count",
        std::function<std::size_t()>([this] { return entity_client_index_.size(); }));
    wr.add<uint64_t>("baseapp/auth_success_total",
                     std::function<uint64_t()>([this] { return auth_success_total_; }));
    wr.add<uint64_t>("baseapp/auth_fail_total",
                     std::function<uint64_t()>([this] { return auth_fail_total_; }));
    wr.add<uint64_t>("baseapp/force_logoff_total",
                     std::function<uint64_t()>([this] { return force_logoff_total_; }));
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
            auto force_logoff_it = pending_force_logoff_writes_.find(msg.request_id);
            if (force_logoff_it != pending_force_logoff_writes_.end())
            {
                const PendingForceLogoffWrite pending_write = force_logoff_it->second;
                pending_force_logoff_writes_.erase(force_logoff_it);

                if (!msg.success)
                {
                    ATLAS_LOG_ERROR(
                        "BaseApp: force-logoff persist failed request_id={} entity_id={} error={}",
                        msg.request_id, pending_write.entity_id, msg.error);

                    auto pending_it = pending_force_logoffs_.find(pending_write.force_request_id);
                    if (pending_it != pending_force_logoffs_.end())
                    {
                        login::PrepareLoginResult reply;
                        reply.request_id = pending_it->second.login_request_id;
                        reply.success = false;
                        reply.error = "force_logoff_persist_failed";
                        send_prepare_login_result(pending_it->second.loginapp_addr, reply);
                        pending_force_logoffs_.erase(pending_it);
                    }
                    return;
                }

                finalize_force_logoff(pending_write.entity_id);
                continue_login_after_force_logoff(pending_write.force_request_id);
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
            // Check if this is a login-flow checkout (request_id is a pending key)
            auto login_it = pending_logins_.find(msg.request_id);
            if (login_it != pending_logins_.end())
            {
                PendingLogin pending = std::move(login_it->second);
                pending_logins_.erase(login_it);

                login::PrepareLoginResult reply;
                reply.request_id = pending.login_request_id;

                if (msg.status != dbapp::CheckoutStatus::Success)
                {
                    ATLAS_LOG_ERROR("BaseApp: login checkout failed (request_id={} status={})",
                                    msg.request_id, static_cast<int>(msg.status));
                    reply.success = false;
                    reply.error = "checkout_failed";
                    send_prepare_login_result(pending.loginapp_addr, reply);
                    return;
                }

                // Create the Proxy entity
                auto* ent = entity_mgr_.create(pending.type_id, true);
                if (!ent)
                {
                    ATLAS_LOG_ERROR(
                        "BaseApp: failed to allocate EntityID for login entity type={} dbid={}",
                        pending.type_id, msg.dbid);

                    if (dbapp_channel_)
                    {
                        dbapp::CheckinEntity checkin;
                        checkin.type_id = pending.type_id;
                        checkin.dbid = msg.dbid;
                        (void)dbapp_channel_->send_message(checkin);
                    }

                    reply.success = false;
                    reply.error = "entity_id_exhausted";
                    send_prepare_login_result(pending.loginapp_addr, reply);
                    return;
                }

                ent->set_entity_data(std::vector<std::byte>(msg.blob.begin(), msg.blob.end()));
                (void)entity_mgr_.assign_dbid(ent->entity_id(), msg.dbid);
                auto* proxy = entity_mgr_.find_proxy(ent->entity_id());
                if (proxy)
                    (void)entity_mgr_.assign_session_key(proxy->entity_id(), pending.session_key);

                if (native_provider_ && native_provider_->restore_entity_fn())
                {
                    native_provider_->restore_entity_fn()(
                        ent->entity_id(), pending.type_id, msg.dbid,
                        reinterpret_cast<const uint8_t*>(msg.blob.data()),
                        static_cast<int32_t>(msg.blob.size()));
                }

                reply.success = true;
                reply.entity_id = ent->entity_id();
                send_prepare_login_result(pending.loginapp_addr, reply);

                ATLAS_LOG_INFO("BaseApp: login entity created id={} dbid={}", ent->entity_id(),
                               msg.dbid);
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
            if (native_provider_ && native_provider_->restore_entity_fn())
            {
                native_provider_->restore_entity_fn()(
                    eid, ent->type_id(), msg.dbid,
                    reinterpret_cast<const uint8_t*>(msg.blob.data()),
                    static_cast<int32_t>(msg.blob.size()));
            }
        });
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

    if (native_provider_)
    {
        if (auto fn = native_provider_->get_entity_data_fn())
        {
            uint8_t* raw = nullptr;
            int32_t len = 0;
            fn(entity_id, &raw, &len);

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
    ATLAS_LOG_WARNING("BaseApp: no managed snapshot callback for entity={}, using cached blob",
                      entity_id);
    return true;
}

void BaseApp::begin_force_logoff_persist(uint32_t force_request_id, EntityID entity_id)
{
    if (!dbapp_channel_)
    {
        ATLAS_LOG_ERROR("BaseApp: force-logoff persist: no DBApp connection");
        auto pending_it = pending_force_logoffs_.find(force_request_id);
        if (pending_it != pending_force_logoffs_.end())
        {
            login::PrepareLoginResult reply;
            reply.request_id = pending_it->second.login_request_id;
            reply.success = false;
            reply.error = "no_dbapp";
            send_prepare_login_result(pending_it->second.loginapp_addr, reply);
            pending_force_logoffs_.erase(pending_it);
        }
        return;
    }

    auto* ent = entity_mgr_.find(entity_id);
    if (!ent || ent->dbid() == kInvalidDBID)
    {
        finalize_force_logoff(entity_id);
        continue_login_after_force_logoff(force_request_id);
        return;
    }

    std::vector<std::byte> blob;
    if (!capture_entity_snapshot(entity_id, blob))
    {
        auto pending_it = pending_force_logoffs_.find(force_request_id);
        if (pending_it != pending_force_logoffs_.end())
        {
            login::PrepareLoginResult reply;
            reply.request_id = pending_it->second.login_request_id;
            reply.success = false;
            reply.error = "force_logoff_snapshot_failed";
            send_prepare_login_result(pending_it->second.loginapp_addr, reply);
            pending_force_logoffs_.erase(pending_it);
        }
        return;
    }

    const uint32_t write_request_id = next_prepare_request_id_++;
    PendingForceLogoffWrite pending_write;
    pending_write.force_request_id = force_request_id;
    pending_write.entity_id = entity_id;
    pending_force_logoff_writes_[write_request_id] = pending_write;

    dbapp::WriteEntity msg;
    msg.flags = WriteFlags::ExplicitDBID | WriteFlags::LogOff;
    msg.type_id = ent->type_id();
    msg.dbid = ent->dbid();
    msg.entity_id = entity_id;
    msg.request_id = write_request_id;
    msg.blob = std::move(blob);
    (void)dbapp_channel_->send_message(msg);
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

    if (!dbapp_channel_)
    {
        login::PrepareLoginResult reply;
        reply.request_id = pending.login_request_id;
        reply.success = false;
        reply.error = "no_dbapp";
        send_prepare_login_result(pending.loginapp_addr, reply);
        return;
    }

    uint32_t new_rid = next_prepare_request_id_++;
    pending_logins_[new_rid] = pending;

    dbapp::CheckoutEntity co;
    co.request_id = new_rid;
    co.dbid = pending.dbid;
    co.type_id = pending.type_id;
    (void)dbapp_channel_->send_message(co);
}

void BaseApp::finalize_force_logoff(EntityID entity_id)
{
    auto* ent = entity_mgr_.find(entity_id);
    if (!ent)
    {
        return;
    }

    unbind_client(entity_id);
    (void)entity_mgr_.clear_session_key(entity_id);
    (void)entity_mgr_.assign_dbid(entity_id, kInvalidDBID);
    if (native_provider_)
    {
        if (auto fn = native_provider_->entity_destroyed_fn())
        {
            fn(entity_id);
        }
    }
    ent->mark_for_destroy();
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
    auto dest_ch_result = network().connect_rudp(dest_baseapp);
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

    auto result = network().connect_rudp(addr);
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
    ATLAS_LOG_INFO("BaseApp: client disconnected from entity={}", entity_id);
}

void BaseApp::send_prepare_login_result(const Address& reply_addr,
                                        const login::PrepareLoginResult& msg)
{
    if (auto* ch = this->resolve_internal_channel(reply_addr))
    {
        (void)ch->send_message(msg);
    }
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
    ATLAS_LOG_INFO("BaseApp: prepare login request_id={} dbid={} type_id={} from {}:{}",
                   msg.request_id, msg.dbid, msg.type_id, ch.remote_address().ip(),
                   ch.remote_address().port());
    // Check if entity with same dbid is already online → force logoff first
    const bool already_online = entity_mgr_.find_by_dbid(msg.dbid) != nullptr;

    if (already_online)
    {
        // Send ForceLogoff to ourselves (within the same BaseApp)
        uint32_t rid = next_prepare_request_id_++;
        PendingLogin pending;
        pending.login_request_id = msg.request_id;
        pending.loginapp_addr = ch.remote_address();
        pending.type_id = msg.type_id;
        pending.dbid = msg.dbid;
        pending.session_key = msg.session_key;
        pending.created_at = Clock::now();
        pending_force_logoffs_[rid] = std::move(pending);

        baseapp::ForceLogoff fo;
        fo.dbid = msg.dbid;
        fo.request_id = rid;
        on_force_logoff(ch, fo);
        return;
    }

    // Create entity from DB
    uint32_t rid = next_prepare_request_id_++;
    PendingLogin pending;
    pending.login_request_id = msg.request_id;
    pending.loginapp_addr = ch.remote_address();
    pending.type_id = msg.type_id;
    pending.dbid = msg.dbid;
    pending.session_key = msg.session_key;
    pending.created_at = Clock::now();
    pending_logins_[rid] = std::move(pending);

    // Load entity from DBApp
    if (dbapp_channel_)
    {
        dbapp::CheckoutEntity co;
        co.request_id = rid;
        co.dbid = msg.dbid;
        co.type_id = msg.type_id;
        (void)dbapp_channel_->send_message(co);
    }
    else
    {
        ATLAS_LOG_ERROR("BaseApp: PrepareLogin: no DBApp connection");
        login::PrepareLoginResult reply;
        reply.request_id = msg.request_id;
        reply.success = false;
        reply.error = "no_dbapp";
        send_prepare_login_result(ch.remote_address(), reply);
    }
    (void)msg.client_addr;
}

void BaseApp::on_force_logoff(Channel& ch, const baseapp::ForceLogoff& msg)
{
    // Find the proxy with this dbid and evict it
    EntityID found_id = kInvalidEntityID;
    if (auto* ent = entity_mgr_.find_by_dbid(msg.dbid))
    {
        found_id = ent->entity_id();
    }

    if (found_id != kInvalidEntityID)
    {
        ++force_logoff_total_;
        ATLAS_LOG_INFO("BaseApp: ForceLogoff: processing entity={} dbid={}", found_id, msg.dbid);
        auto pending_it = pending_force_logoffs_.find(msg.request_id);
        if (pending_it != pending_force_logoffs_.end())
        {
            begin_force_logoff_persist(msg.request_id, found_id);
            return;
        }

        finalize_force_logoff(found_id);
    }

    auto pending_it = pending_force_logoffs_.find(msg.request_id);
    if (pending_it != pending_force_logoffs_.end())
    {
        continue_login_after_force_logoff(msg.request_id);
        return;
    }

    baseapp::ForceLogoffAck ack;
    ack.request_id = msg.request_id;
    ack.success = true;
    (void)ch.send_message(ack);
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
        pending_force_logoffs_.erase(it);
        return;
    }

    // Logoff succeeded — now checkout entity from DBApp
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

    ATLAS_LOG_INFO("BaseApp: client authenticated as entity={}", proxy->entity_id());
}

}  // namespace atlas
