#pragma once

#include "baseapp_native_provider.hpp"
#include "db/idatabase.hpp"
#include "entity_manager.hpp"
#include "foundation/time.hpp"
#include "server/entity_app.hpp"
#include "server/entity_types.hpp"

#include <memory>
#include <unordered_map>

namespace atlas
{

namespace baseapp
{
struct CreateBase;
struct CreateBaseFromDB;
struct AcceptClient;
struct CellEntityCreated;
struct CellEntityDestroyed;
struct CurrentCell;
struct CellRpcForward;
struct SelfRpcFromCell;
struct BroadcastRpcFromCell;
struct ReplicatedDeltaFromCell;
struct ForceLogoff;
struct ForceLogoffAck;
struct Authenticate;
}  // namespace baseapp

namespace login
{
struct PrepareLogin;
struct PrepareLoginResult;
}  // namespace login

namespace baseappmgr
{
struct RegisterBaseAppAck;
struct RequestEntityIdRangeAck;
}  // namespace baseappmgr

class Channel;

// ============================================================================
// BaseApp — entity-bearing server process
//
// Responsibilities:
//   • Hosts BaseEntity / Proxy objects
//   • Dual network interfaces: internal (peer servers) + external (clients)
//   • Allocates EntityIDs locally (EntityManager)
//   • Routes client RPCs from external interface to entities
//   • Forwards DB persistence requests to DBApp
//   • Implements give_client_to (local + remote)
//   • Scripted entity logic via ClrScriptEngine / BaseAppNativeProvider
// ============================================================================

class BaseApp : public EntityApp
{
public:
    static auto run(int argc, char* argv[]) -> int;

    BaseApp(EventDispatcher& dispatcher, NetworkInterface& internal_network,
            NetworkInterface& external_network);
    ~BaseApp() override;

    [[nodiscard]] auto entity_manager() -> EntityManager& { return entity_mgr_; }
    [[nodiscard]] auto native_provider() -> BaseAppNativeProvider& { return *native_provider_; }

protected:
    // ---- EntityApp / ScriptApp overrides --------------------------------
    [[nodiscard]] auto init(int argc, char* argv[]) -> bool override;
    void fini() override;

    void on_tick_complete() override;
    void register_watchers() override;

    [[nodiscard]] auto create_native_provider() -> std::unique_ptr<INativeApiProvider> override;

private:
    // ---- Message handlers — internal interface --------------------------
    void on_create_base(Channel& ch, const baseapp::CreateBase& msg);
    void on_create_base_from_db(Channel& ch, const baseapp::CreateBaseFromDB& msg);
    void on_accept_client(Channel& ch, const baseapp::AcceptClient& msg);
    void on_cell_entity_created(Channel& ch, const baseapp::CellEntityCreated& msg);
    void on_cell_entity_destroyed(Channel& ch, const baseapp::CellEntityDestroyed& msg);
    void on_current_cell(Channel& ch, const baseapp::CurrentCell& msg);
    void on_cell_rpc_forward(Channel& ch, const baseapp::CellRpcForward& msg);
    void on_self_rpc_from_cell(Channel& ch, const baseapp::SelfRpcFromCell& msg);
    void on_broadcast_rpc_from_cell(Channel& ch, const baseapp::BroadcastRpcFromCell& msg);
    void on_replicated_delta_from_cell(Channel& ch, const baseapp::ReplicatedDeltaFromCell& msg);

    // ---- Login flow handlers --------------------------------------------
    void on_prepare_login(Channel& ch, const login::PrepareLogin& msg);
    void on_force_logoff(Channel& ch, const baseapp::ForceLogoff& msg);
    void on_force_logoff_ack(Channel& ch, const baseapp::ForceLogoffAck& msg);
    void on_register_baseapp_ack(Channel& ch, const baseappmgr::RegisterBaseAppAck& msg);
    void on_request_entity_id_range_ack(Channel& ch,
                                        const baseappmgr::RequestEntityIdRangeAck& msg);

    // ---- External client handler ----------------------------------------
    void on_client_authenticate(Channel& ch, const baseapp::Authenticate& msg);

    // ---- Called by BaseAppNativeProvider --------------------------------
    friend class BaseAppNativeProvider;
    void do_write_to_db(EntityID entity_id, const std::byte* data, int32_t len);
    void do_give_client_to_local(EntityID src_id, EntityID dest_id);
    void do_give_client_to_remote(EntityID src_id, EntityID dest_id, const Address& dest_baseapp);

    // ---- Helpers --------------------------------------------------------
    void register_internal_handlers();
    void send_to_dbapp(Channel*& dbapp_ch, auto&& msg);
    void maybe_request_more_ids();
    auto capture_entity_snapshot(EntityID entity_id, std::vector<std::byte>& out) -> bool;
    void begin_force_logoff_persist(uint32_t force_request_id, EntityID entity_id);
    void continue_login_after_force_logoff(uint32_t force_request_id);
    void finalize_force_logoff(EntityID entity_id);
    auto resolve_internal_channel(const Address& addr) -> Channel*;
    auto resolve_client_channel(EntityID entity_id) -> Channel*;
    auto bind_client(EntityID entity_id, const Address& client_addr) -> bool;
    void unbind_client(EntityID entity_id);
    void on_external_client_disconnect(Channel& ch);
    void send_prepare_login_result(const Address& reply_addr, const login::PrepareLoginResult& msg);

    // ---- State ----------------------------------------------------------
    NetworkInterface& external_network_;
    EntityManager entity_mgr_;
    BaseAppNativeProvider* native_provider_{nullptr};  // owned by ScriptApp
    Channel* dbapp_channel_{nullptr};                  // connection to DBApp
    Channel* baseappmgr_channel_{nullptr};             // connection to BaseAppMgr
    uint32_t app_id_{0};

    // Pending login state: maps request_id → reply channel back to LoginApp
    struct PendingLogin
    {
        uint32_t login_request_id{0};
        Address loginapp_addr;
        uint16_t type_id{0};
        DatabaseID dbid{kInvalidDBID};
        SessionKey session_key;
        TimePoint created_at{};
    };
    std::unordered_map<uint32_t, PendingLogin> pending_logins_;
    uint32_t next_prepare_request_id_{1};

    // Pending ForceLogoff awaiting ack: maps request_id → PendingLogin
    std::unordered_map<uint32_t, PendingLogin> pending_force_logoffs_;
    struct PendingForceLogoffWrite
    {
        uint32_t force_request_id{0};
        EntityID entity_id{kInvalidEntityID};
    };
    std::unordered_map<uint32_t, PendingForceLogoffWrite> pending_force_logoff_writes_;
    std::unordered_map<EntityID, Address> entity_client_index_;
    std::unordered_map<Address, EntityID> client_entity_index_;
    uint64_t auth_success_total_{0};
    uint64_t auth_fail_total_{0};
    uint64_t force_logoff_total_{0};
    bool id_range_requested_{false};
};

}  // namespace atlas
