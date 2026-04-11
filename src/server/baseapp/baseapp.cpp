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
            auto* ent = entity_mgr_.find(msg.request_id);
            if (ent)
                ent->on_write_ack(msg.dbid, msg.success);
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
                reply.request_id = msg.request_id;

                if (msg.status != dbapp::CheckoutStatus::Success)
                {
                    ATLAS_LOG_ERROR("BaseApp: login checkout failed (request_id={} status={})",
                                    msg.request_id, static_cast<int>(msg.status));
                    reply.success = false;
                    reply.error = "checkout_failed";
                    if (pending.loginapp_ch)
                        (void)pending.loginapp_ch->send_message(reply);
                    return;
                }

                // Create the Proxy entity
                auto* ent = entity_mgr_.create(pending.type_id, true);
                ent->set_entity_data(std::vector<std::byte>(msg.blob.begin(), msg.blob.end()));
                ent->set_dbid(msg.dbid);
                auto* proxy = entity_mgr_.find_proxy(ent->entity_id());
                if (proxy)
                    proxy->set_session_key(pending.session_key);

                if (native_provider_ && native_provider_->restore_entity_fn())
                {
                    native_provider_->restore_entity_fn()(
                        ent->entity_id(), pending.type_id, msg.dbid,
                        reinterpret_cast<const uint8_t*>(msg.blob.data()),
                        static_cast<int32_t>(msg.blob.size()));
                }

                reply.success = true;
                reply.entity_id = ent->entity_id();
                if (pending.loginapp_ch)
                    (void)pending.loginapp_ch->send_message(reply);

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
    proxy->set_session_key(msg.session_key);
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

    (void)proxy->client_channel()->send_message(
        static_cast<MessageID>(msg.rpc_id),
        std::span<const std::byte>(msg.payload.data(), msg.payload.size()));
}

void BaseApp::on_broadcast_rpc_from_cell(Channel& /*ch*/, const baseapp::BroadcastRpcFromCell& msg)
{
    auto* proxy = entity_mgr_.find_proxy(msg.base_entity_id);
    if (!proxy || !proxy->has_client())
        return;

    (void)proxy->client_channel()->send_message(
        static_cast<MessageID>(msg.rpc_id),
        std::span<const std::byte>(msg.payload.data(), msg.payload.size()));
}

void BaseApp::on_replicated_delta_from_cell(Channel& /*ch*/,
                                            const baseapp::ReplicatedDeltaFromCell& msg)
{
    auto* proxy = entity_mgr_.find_proxy(msg.base_entity_id);
    if (!proxy || !proxy->has_client())
        return;

    // Forward replicated delta to client (reserved update message ID)
    (void)proxy->client_channel()->send_message(
        static_cast<MessageID>(0xF001),
        std::span<const std::byte>(msg.delta.data(), msg.delta.size()));
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
    dst_proxy->set_session_key(src_proxy->session_key());
    dst_proxy->set_client_channel(src_proxy->client_channel());
    src_proxy->set_client_channel(nullptr);
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

    src_proxy->set_client_channel(nullptr);
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
    // Check if entity with same dbid is already online → force logoff first
    bool already_online = false;
    entity_mgr_.for_each(
        [&](const BaseEntity& ent)
        {
            if (ent.dbid() == msg.dbid)
                already_online = true;
        });

    if (already_online)
    {
        // Send ForceLogoff to ourselves (within the same BaseApp)
        uint32_t rid = next_prepare_request_id_++;
        PendingLogin pending;
        pending.loginapp_ch = &ch;
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
    pending.loginapp_ch = &ch;
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
        (void)ch.send_message(reply);
    }
    (void)msg.client_addr;
}

void BaseApp::on_force_logoff(Channel& /*ch*/, const baseapp::ForceLogoff& msg)
{
    // Find the proxy with this dbid and evict it
    EntityID found_id = kInvalidEntityID;
    entity_mgr_.for_each(
        [&](const BaseEntity& ent)
        {
            if (ent.dbid() == msg.dbid)
                found_id = ent.entity_id();
        });

    if (found_id != kInvalidEntityID)
    {
        auto* ent = entity_mgr_.find(found_id);
        if (ent)
            ent->mark_for_destroy();
        ATLAS_LOG_INFO("BaseApp: ForceLogoff: evicted entity={} dbid={}", found_id, msg.dbid);
    }

    // Send ack back to caller
    baseapp::ForceLogoffAck ack;
    ack.request_id = msg.request_id;
    ack.success = true;

    auto it = pending_force_logoffs_.find(msg.request_id);
    if (it != pending_force_logoffs_.end())
    {
        // Self-initiated logoff — continue checkout
        PendingLogin& pending = it->second;
        if (dbapp_channel_)
        {
            uint32_t new_rid = next_prepare_request_id_++;
            pending_logins_[new_rid] = pending;

            dbapp::CheckoutEntity co;
            co.request_id = new_rid;
            co.dbid = pending.dbid;
            co.type_id = pending.type_id;
            (void)dbapp_channel_->send_message(co);
        }
        pending_force_logoffs_.erase(it);
    }
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
        reply.request_id = msg.request_id;
        reply.success = false;
        reply.error = "force_logoff_failed";
        if (pending.loginapp_ch)
            (void)pending.loginapp_ch->send_message(reply);
        pending_force_logoffs_.erase(it);
        return;
    }

    // Logoff succeeded — now checkout entity from DBApp
    PendingLogin& pending = it->second;
    uint32_t new_rid = next_prepare_request_id_++;
    pending_logins_[new_rid] = pending;

    if (dbapp_channel_)
    {
        dbapp::CheckoutEntity co;
        co.request_id = new_rid;
        co.dbid = pending.dbid;
        co.type_id = pending.type_id;
        (void)dbapp_channel_->send_message(co);
    }
    pending_force_logoffs_.erase(it);
}

// ============================================================================
// on_client_authenticate — handle first message from a client
// ============================================================================

void BaseApp::on_client_authenticate(Channel& ch, const baseapp::Authenticate& msg)
{
    // Look for a Proxy with the matching session key
    EntityID found_id = kInvalidEntityID;
    entity_mgr_.for_each(
        [&](const BaseEntity& ent)
        {
            auto* proxy = dynamic_cast<const Proxy*>(&ent);
            if (proxy && proxy->session_key() == msg.session_key)
                found_id = ent.entity_id();
        });

    if (found_id == kInvalidEntityID)
    {
        ATLAS_LOG_WARNING("BaseApp: Authenticate: no matching session key");
        baseapp::AuthenticateResult res;
        res.success = false;
        res.error = "invalid_session";
        (void)ch.send_message(res);
        return;
    }

    auto* proxy = entity_mgr_.find_proxy(found_id);
    if (!proxy)
    {
        baseapp::AuthenticateResult res;
        res.success = false;
        res.error = "not_a_proxy";
        (void)ch.send_message(res);
        return;
    }

    proxy->set_client_channel(&ch);

    baseapp::AuthenticateResult res;
    res.success = true;
    res.entity_id = found_id;
    res.type_id = proxy->type_id();
    (void)ch.send_message(res);

    ATLAS_LOG_INFO("BaseApp: client authenticated as entity={}", found_id);
}

}  // namespace atlas
