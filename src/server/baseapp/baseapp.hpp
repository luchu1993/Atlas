#pragma once

#include "baseapp_native_provider.hpp"
#include "db/idatabase.hpp"
#include "entity_manager.hpp"
#include "foundation/time.hpp"
#include "server/entity_app.hpp"
#include "server/entity_types.hpp"

#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
struct InformLoad;
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

    void on_end_of_tick() override;
    void on_tick_complete() override;
    void register_watchers() override;

    [[nodiscard]] auto create_native_provider() -> std::unique_ptr<INativeApiProvider> override;

private:
    struct LoadSnapshot;
    struct LoadTracker;

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
    void expire_detached_proxies();
    void update_load_estimate();
    void report_load_to_baseappmgr();
    [[nodiscard]] auto capture_load_snapshot() const -> LoadSnapshot;
    void drain_finished_login_flows(std::vector<DatabaseID> dbids);
    void maybe_request_more_ids();

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
        TimePoint next_force_logoff_retry_at{};
        Address force_logoff_holder_addr;
        uint8_t force_logoff_retry_count{0};
        bool waiting_for_remote_force_logoff_ack{false};
        bool reply_sent{false};
    };
    std::unordered_map<uint32_t, PendingLogin> pending_logins_;
    uint32_t next_prepare_request_id_{1};

    // Pending ForceLogoff awaiting ack: maps request_id → PendingLogin
    std::unordered_map<uint32_t, PendingLogin> pending_force_logoffs_;
    struct PendingLogoffWrite
    {
        uint32_t continuation_request_id{0};
        EntityID entity_id{kInvalidEntityID};
        DatabaseID dbid{kInvalidDBID};
        uint16_t type_id{0};
        TimePoint created_at{};
    };
    struct PendingRemoteForceLogoffAck
    {
        Address reply_addr;
        uint32_t request_id{0};
    };
    struct DeferredLoginCheckout
    {
        PendingLogin pending;
        DatabaseID dbid{kInvalidDBID};
        uint16_t type_id{0};
        std::vector<std::byte> blob;
    };
    struct DetachedProxyState
    {
        TimePoint detached_at{};
        TimePoint detached_until{};
    };
    struct LoadSnapshot
    {
        uint32_t entity_count{0};
        uint32_t proxy_count{0};
        uint32_t pending_prepare_count{0};
        uint32_t pending_force_logoff_count{0};
        uint32_t detached_proxy_count{0};
        uint32_t logoff_in_flight_count{0};
        uint32_t deferred_login_count{0};
    };
    struct LoadTracker
    {
        void mark_tick_started();
        void observe_tick_complete(int update_hertz, const LoadSnapshot& snapshot);
        [[nodiscard]] auto build_report(uint32_t app_id, const LoadSnapshot& snapshot) const
            -> baseappmgr::InformLoad;
        [[nodiscard]] auto current_load() const -> float { return load_; }

    private:
        float load_{0.0f};
        TimePoint tick_started_{};
    };
    std::unordered_map<uint32_t, PendingLogoffWrite> pending_logoff_writes_;
    std::unordered_map<EntityID, std::vector<PendingRemoteForceLogoffAck>>
        pending_remote_force_logoff_acks_;
    std::unordered_map<EntityID, std::vector<DeferredLoginCheckout>> deferred_login_checkouts_;
    std::unordered_map<EntityID, DetachedProxyState> detached_proxies_;
    std::unordered_map<EntityID, std::vector<uint32_t>> pending_local_force_logoff_waiters_;
    std::unordered_set<EntityID> logoff_entities_in_flight_;
    std::unordered_set<DatabaseID> active_login_dbids_;
    std::unordered_map<DatabaseID, PendingLogin> queued_logins_;
    std::unordered_map<EntityID, Address> entity_client_index_;
    std::unordered_map<Address, EntityID> client_entity_index_;
    uint64_t auth_success_total_{0};
    uint64_t auth_fail_total_{0};
    uint64_t force_logoff_total_{0};
    uint64_t fast_relogin_total_{0};
    uint64_t detached_relogin_total_{0};
    LoadTracker load_tracker_{};
    static constexpr Duration kForceLogoffRetryBaseDelay = std::chrono::milliseconds(250);
    static constexpr Duration kForceLogoffRetryMaxDelay = std::chrono::seconds(2);
    static constexpr Duration kPendingTimeout = std::chrono::seconds(30);
    // Keep detached proxies around for one shortline reconnect window. Longer
    // retention increases stale proxy pressure without improving the fast path.
    static constexpr Duration kDetachedProxyGrace = std::chrono::milliseconds(1500);
    static constexpr float kLoadSmoothingBias = 0.25f;
    bool id_range_requested_{false};

    void cleanup_expired_pending_requests();
    void fail_all_dbapp_pending_requests(std::string_view reason);
    void fail_pending_prepare_login(PendingLogin& pending, std::string_view reason);
    void fail_pending_prepare_login(uint32_t request_id, std::string_view reason);
    void fail_pending_force_logoff(PendingLogin& pending, std::string_view reason);
    void fail_pending_force_logoff(uint32_t request_id, std::string_view reason);
    void schedule_force_logoff_retry(PendingLogin& pending, TimePoint now);
    void retry_stalled_force_logoff(uint32_t request_id);
    void release_checkout(DatabaseID dbid, uint16_t type_id);
    [[nodiscard]] auto retry_login_after_checkout_conflict(PendingLogin pending, DatabaseID dbid,
                                                           const Address& holder_addr) -> bool;
    [[nodiscard]] auto restore_managed_entity(EntityID entity_id, uint16_t type_id, DatabaseID dbid,
                                              std::span<const std::byte> blob) -> bool;
    [[nodiscard]] auto notify_managed_entity_destroyed(EntityID entity_id, std::string_view context)
        -> bool;
    auto capture_entity_snapshot(EntityID entity_id, std::vector<std::byte>& out) -> bool;
    [[nodiscard]] auto rotate_proxy_session(EntityID entity_id, const SessionKey& session_key)
        -> bool;
    [[nodiscard]] auto try_complete_local_relogin(PendingLogin pending) -> bool;
    void enter_detached_grace(EntityID entity_id);
    void clear_detached_grace(EntityID entity_id);
    [[nodiscard]] auto deferred_login_checkout_count() const -> std::size_t;
    void complete_prepare_login_from_checkout(PendingLogin pending, DatabaseID dbid,
                                              uint16_t type_id, std::span<const std::byte> blob);
    void defer_prepare_login_from_checkout(EntityID blocking_entity_id, PendingLogin pending,
                                           DatabaseID dbid, uint16_t type_id,
                                           std::span<const std::byte> blob);
    void fail_deferred_prepare_logins(EntityID blocking_entity_id, std::string_view reason,
                                      std::vector<DatabaseID>* finished_dbids = nullptr);
    [[nodiscard]] auto resume_deferred_prepare_logins(EntityID blocking_entity_id) -> bool;
    void submit_prepare_login(PendingLogin pending);
    void dispatch_prepare_login(PendingLogin pending);
    void finish_login_flow(DatabaseID dbid);
    void start_disconnect_logoff(EntityID entity_id);
    void flush_remote_force_logoff_acks(EntityID entity_id, bool success);
    void flush_all_remote_force_logoff_acks(bool success);
    void begin_logoff_persist(EntityID entity_id, DatabaseID dbid, uint16_t type_id,
                              uint32_t continuation_request_id);
    void begin_force_logoff_persist(uint32_t force_request_id, EntityID entity_id);
    void continue_login_after_force_logoff(uint32_t force_request_id);
    [[nodiscard]] auto finalize_force_logoff(EntityID entity_id) -> bool;
    void process_force_logoff_request(const baseapp::ForceLogoff& msg);
    auto resolve_internal_channel(const Address& addr) -> Channel*;
    auto resolve_client_channel(EntityID entity_id) -> Channel*;
    auto bind_client(EntityID entity_id, const Address& client_addr) -> bool;
    void unbind_client(EntityID entity_id);
    void on_external_client_disconnect(Channel& ch);
    void send_prepare_login_result(const Address& reply_addr, const login::PrepareLoginResult& msg);
};

}  // namespace atlas
