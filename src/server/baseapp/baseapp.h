#ifndef ATLAS_SERVER_BASEAPP_BASEAPP_H_
#define ATLAS_SERVER_BASEAPP_BASEAPP_H_

#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "baseapp_native_provider.h"
#include "db/idatabase.h"
#include "dbapp/dbapp_messages.h"
#include "delta_forwarder.h"
#include "entity_manager.h"
#include "foundation/clock.h"
#include "id_client.h"
#include "server/cellapp_peer_registry.h"
#include "server/entity_app.h"
#include "server/entity_types.h"

namespace atlas {

namespace baseapp {
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
struct ReplicatedReliableDeltaFromCell;
struct ForceLogoff;
struct ForceLogoffAck;
struct Authenticate;
struct ClientBaseRpc;
struct ClientCellRpc;
}  // namespace baseapp

namespace login {
struct PrepareLogin;
struct PrepareLoginResult;
struct CancelPrepareLogin;
}  // namespace login

namespace baseappmgr {
struct InformLoad;
struct RegisterBaseAppAck;
}  // namespace baseappmgr

namespace cellappmgr {
struct SpaceCreatedResult;
}  // namespace cellappmgr

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

// ============================================================================
// Phase 11 PR-6 routing helper (pure function)
// ============================================================================
//
// Looks up a CellApp Channel* for `cell_addr` in the peer-channel map.
// Returns nullptr when:
//   - cell_addr has port 0 (default-constructed Address; the entity has
//     not yet been bound to a Cell via OnCellEntityCreated / OnCurrentCell);
//   - the map has no entry for cell_addr (peer CellApp not known, either
//     not yet connected or already died).
//
// Exposed at namespace scope so unit tests can drive it on a plain map +
// synthetic Channel* values without having to construct a BaseApp. The
// complete "entity → cell_addr → channel" chain is covered by
// BaseApp::ResolveCellChannelForEntity which layers the entity lookup
// on top of this helper.

[[nodiscard]] auto ResolveCellChannelByAddr(
    const std::unordered_map<Address, Channel*>& cellapp_channels, const Address& cell_addr)
    -> Channel*;

class BaseApp : public EntityApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  BaseApp(EventDispatcher& dispatcher, NetworkInterface& internal_network,
          NetworkInterface& external_network);
  ~BaseApp() override;

  [[nodiscard]] auto GetEntityManager() -> EntityManager& { return entity_mgr_; }
  [[nodiscard]] auto GetNativeProvider() -> BaseAppNativeProvider& { return *native_provider_; }

  // ---- Phase 11 PR-6 CellApp routing ------------------------------------
  //
  // Returns the CellApp channel that currently owns `target_entity_id`'s
  // Real, or nullptr if the entity is unknown, has no Cell yet, or the
  // peer channel map has no entry for its current cell_addr. Used by
  // OnClientCellRpc. Wraps the pure ResolveCellChannelByAddr helper
  // (see free-function declaration below) with the entity lookup.
  [[nodiscard]] auto ResolveCellChannelForEntity(EntityID target_entity_id) const -> Channel*;

  // ---- Phase 11 PR-6 review-fix S2/S3: Space creation via CellAppMgr ----
  //
  // Callback invoked when CellAppMgr replies to a CreateSpaceRequest.
  // `success` indicates whether the Space exists afterwards; on success
  // `cell_addr` holds the CellApp that now hosts the initial Cell.
  // Scripts/game logic hook in via RequestCreateSpace() below.
  using SpaceCreatedCallback =
      std::function<void(bool success, SpaceID space_id, const Address& cell_addr)>;

  // Requests a new Space from CellAppMgr. Returns 0 if no CellAppMgr is
  // currently connected (birth hasn't happened yet or it died); caller
  // can retry. Non-zero return is the request_id assigned by BaseApp —
  // the callback fires when CellAppMgr replies (or never, on timeout;
  // the pending table is pruned by a periodic sweep).
  auto RequestCreateSpace(SpaceID space_id, SpaceCreatedCallback callback) -> uint32_t;

 protected:
  // ---- EntityApp / ScriptApp overrides --------------------------------
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;

  void OnEndOfTick() override;
  void OnTickComplete() override;
  void RegisterWatchers() override;

  [[nodiscard]] auto CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> override;

 private:
  friend class BaseAppRollbackTest;

  struct LoadSnapshot;
  struct LoadTracker;

  // ---- Message handlers — internal interface --------------------------
  void OnCreateBase(Channel& ch, const baseapp::CreateBase& msg);
  void OnCreateBaseFromDb(Channel& ch, const baseapp::CreateBaseFromDB& msg);
  void OnAcceptClient(Channel& ch, const baseapp::AcceptClient& msg);
  void OnCellEntityCreated(Channel& ch, const baseapp::CellEntityCreated& msg);
  void OnCellEntityDestroyed(Channel& ch, const baseapp::CellEntityDestroyed& msg);
  void OnCurrentCell(Channel& ch, const baseapp::CurrentCell& msg);
  void OnCellRpcForward(Channel& ch, const baseapp::CellRpcForward& msg);
  void OnSelfRpcFromCell(Channel& ch, const baseapp::SelfRpcFromCell& msg);
  void OnBroadcastRpcFromCell(Channel& ch, const baseapp::BroadcastRpcFromCell& msg);
  void OnReplicatedDeltaFromCell(Channel& ch, const baseapp::ReplicatedDeltaFromCell& msg);
  void OnReplicatedReliableDeltaFromCell(Channel& ch,
                                         const baseapp::ReplicatedReliableDeltaFromCell& msg);
  // Phase 11 review-fix S2/S3.
  void OnSpaceCreatedResult(Channel& ch, const cellappmgr::SpaceCreatedResult& msg);

  // ---- Login flow handlers --------------------------------------------
  void OnPrepareLogin(Channel& ch, const login::PrepareLogin& msg);
  void OnCancelPrepareLogin(Channel& ch, const login::CancelPrepareLogin& msg);
  void OnForceLogoff(Channel& ch, const baseapp::ForceLogoff& msg);
  void OnForceLogoffAck(Channel& ch, const baseapp::ForceLogoffAck& msg);
  void OnRegisterBaseappAck(Channel& ch, const baseappmgr::RegisterBaseAppAck& msg);
  void OnGetEntityIdsAck(Channel& ch, const dbapp::GetEntityIdsAck& msg);

  // ---- External client handler ----------------------------------------
  void OnClientAuthenticate(Channel& ch, const baseapp::Authenticate& msg);
  void OnClientBaseRpc(Channel& ch, const baseapp::ClientBaseRpc& msg);
  void OnClientCellRpc(Channel& ch, const baseapp::ClientCellRpc& msg);

  // ---- Called by BaseAppNativeProvider --------------------------------
  friend class BaseAppNativeProvider;
  void DoWriteToDb(EntityID entity_id, const std::byte* data, int32_t len);
  void DoGiveClientToLocal(EntityID src_id, EntityID dest_id);
  void DoGiveClientToRemote(EntityID src_id, EntityID dest_id, const Address& dest_baseapp);

  // ---- Delta forwarding ------------------------------------------------
  void FlushClientDeltas();

  // ---- Baseline snapshot emission -------------------------------------
  // Called each tick; sends a reliable full-state snapshot for every live
  // client-bound entity once per kBaselineInterval ticks. Ensures that UDP
  // loss on the unreliable delta path cannot leave the client permanently
  // stale — the next baseline reconverges state within one interval.
  void EmitBaselineSnapshots();

  // ---- Helpers --------------------------------------------------------
  void RegisterInternalHandlers();
  void ExpireDetachedProxies();
  void UpdateLoadEstimate();
  void ReportLoadToBaseAppMgr();
  [[nodiscard]] auto CaptureLoadSnapshot() const -> LoadSnapshot;
  void DrainFinishedLoginFlows(std::vector<DatabaseID> dbids);
  void MaybeRequestMoreIds();

  // ---- State ----------------------------------------------------------
  NetworkInterface& external_network_;
  IDClient id_client_;
  EntityManager entity_mgr_;
  BaseAppNativeProvider* native_provider_{nullptr};  // owned by ScriptApp
  Channel* dbapp_channel_{nullptr};                  // connection to DBApp
  Channel* baseappmgr_channel_{nullptr};             // connection to BaseAppMgr
  // Phase 11 PR-6 / review-fix C2: multi-CellApp routing. The registry
  // handles Birth/Death subscription + self-filter internally. Per-
  // entity routing (which CellApp this entity's Real currently lives
  // on) lives on BaseEntity.cell_addr_, maintained by
  // OnCellEntityCreated + OnCurrentCell.
  CellAppPeerRegistry cellapp_peers_;

  // Phase 11 PR-6 review-fix S2/S3: CellAppMgr control channel +
  // pending Space-create callbacks keyed by request_id.
  Channel* cellappmgr_channel_{nullptr};
  uint32_t next_space_request_id_{1};
  std::unordered_map<uint32_t, SpaceCreatedCallback> pending_space_creates_;

  uint32_t app_id_{0};

  // Pending login state: maps request_id → reply channel back to LoginApp
  struct PendingLogin {
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
    bool blob_prefetched{false};
    std::vector<std::byte> entity_blob;
  };
  std::unordered_map<uint32_t, PendingLogin> pending_logins_;
  uint32_t next_prepare_request_id_{1};

  // Pending ForceLogoff awaiting ack: maps request_id → PendingLogin
  std::unordered_map<uint32_t, PendingLogin> pending_force_logoffs_;
  struct PendingLogoffWrite {
    uint32_t continuation_request_id{0};
    EntityID entity_id{kInvalidEntityID};
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    TimePoint created_at{};
  };
  struct PendingRemoteForceLogoffAck {
    Address reply_addr;
    uint32_t request_id{0};
  };
  struct DeferredLoginCheckout {
    PendingLogin pending;
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    std::vector<std::byte> blob;
  };
  struct DetachedProxyState {
    TimePoint detached_at{};
    TimePoint detached_until{};
  };
  struct PreparedLoginEntity {
    EntityID entity_id{kInvalidEntityID};
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    TimePoint prepared_at{};
  };
  struct CanceledCheckout {
    DatabaseID dbid{kInvalidDBID};
    uint16_t type_id{0};
    TimePoint canceled_at{};
  };
  struct LoadSnapshot {
    uint32_t entity_count{0};
    uint32_t proxy_count{0};
    uint32_t pending_prepare_count{0};
    uint32_t pending_force_logoff_count{0};
    uint32_t detached_proxy_count{0};
    uint32_t logoff_in_flight_count{0};
    uint32_t deferred_login_count{0};
  };
  struct LoadTracker {
    void MarkTickStarted();
    void ObserveTickComplete(int update_hertz, const LoadSnapshot& snapshot);
    [[nodiscard]] auto BuildReport(uint32_t app_id, const LoadSnapshot& snapshot) const
        -> baseappmgr::InformLoad;
    [[nodiscard]] auto CurrentLoad() const -> float { return load_; }

   private:
    float load_{0.0f};
    TimePoint tick_started_{};
  };
  std::unordered_map<uint32_t, PendingLogoffWrite> pending_logoff_writes_;
  std::unordered_map<EntityID, std::vector<PendingRemoteForceLogoffAck>>
      pending_remote_force_logoff_acks_;
  std::unordered_map<EntityID, std::vector<DeferredLoginCheckout>> deferred_login_checkouts_;
  std::unordered_map<EntityID, DetachedProxyState> detached_proxies_;
  std::unordered_map<uint32_t, PreparedLoginEntity> prepared_login_entities_;
  std::unordered_map<EntityID, uint32_t> prepared_login_requests_by_entity_;
  std::unordered_map<uint32_t, CanceledCheckout> canceled_login_checkouts_;
  std::unordered_map<EntityID, std::vector<uint32_t>> pending_local_force_logoff_waiters_;
  std::unordered_set<EntityID> logoff_entities_in_flight_;
  std::unordered_set<DatabaseID> active_login_dbids_;
  std::unordered_map<DatabaseID, PendingLogin> queued_logins_;
  std::unordered_map<EntityID, Address> entity_client_index_;
  std::unordered_map<Address, EntityID> client_entity_index_;
  std::unordered_map<Address, DeltaForwarder> client_delta_forwarders_;
  uint64_t delta_bytes_sent_total_{0};
  uint64_t delta_bytes_deferred_total_{0};
  uint64_t reliable_delta_bytes_sent_total_{0};
  uint64_t reliable_delta_messages_sent_total_{0};
  uint64_t baseline_messages_sent_total_{0};
  uint64_t baseline_bytes_sent_total_{0};
  uint64_t baseline_tick_counter_{0};
  static constexpr uint32_t kDeltaBudgetPerTick = 16 * 1024;  // 16 KB per client per tick
  // ~30 ticks (≈ 1 s at 30 Hz) between reliable full-state snapshots per entity.
  // Compensates补强二's DeltaForwarder unreliable losses within one interval.
  static constexpr uint64_t kBaselineInterval = 30;
  uint64_t auth_success_total_{0};
  uint64_t auth_fail_total_{0};
  uint64_t force_logoff_total_{0};
  uint64_t fast_relogin_total_{0};
  uint64_t detached_relogin_total_{0};
  uint64_t canceled_checkout_total_{0};
  uint64_t prepared_login_timeout_total_{0};
  LoadTracker load_tracker_{};
  static constexpr Duration kForceLogoffRetryBaseDelay = std::chrono::milliseconds(250);
  static constexpr Duration kForceLogoffRetryMaxDelay = std::chrono::seconds(2);
  static constexpr Duration kPendingTimeout = std::chrono::seconds(8);
  static constexpr Duration kCanceledCheckoutRetention = std::chrono::seconds(10);
  static constexpr Duration kPreparedLoginTimeout = std::chrono::seconds(10);
  // Keep detached proxies around for one shortline reconnect window. Longer
  // retention increases stale proxy pressure without improving the fast path.
  static constexpr Duration kDetachedProxyGrace = std::chrono::milliseconds(1500);
  static constexpr float kLoadSmoothingBias = 0.25f;

  void CleanupExpiredPendingRequests();
  void FailAllDbappPendingRequests(std::string_view reason);
  void FailPendingPrepareLogin(PendingLogin& pending, std::string_view reason);
  void FailPendingPrepareLogin(uint32_t request_id, std::string_view reason);
  void FailPendingForceLogoff(PendingLogin& pending, std::string_view reason);
  void FailPendingForceLogoff(uint32_t request_id, std::string_view reason);
  void ScheduleForceLogoffRetry(PendingLogin& pending, TimePoint now);
  void RetryStalledForceLogoff(uint32_t request_id);
  void ReleaseCheckout(DatabaseID dbid, uint16_t type_id);
  void CancelInflightCheckout(uint32_t request_id, const PendingLogin& pending);
  void SendAbortCheckout(uint32_t request_id, DatabaseID dbid, uint16_t type_id);
  void CancelPrepareLogin(uint32_t login_request_id, DatabaseID dbid);
  [[nodiscard]] auto RollbackPreparedLoginEntity(uint32_t login_request_id) -> bool;
  void ClearPreparedLoginEntity(EntityID entity_id);
  [[nodiscard]] auto RetryLoginAfterCheckoutConflict(PendingLogin pending, DatabaseID dbid,
                                                     const Address& holder_addr) -> bool;
  [[nodiscard]] auto RestoreManagedEntity(EntityID entity_id, uint16_t type_id, DatabaseID dbid,
                                          std::span<const std::byte> blob) -> bool;
  [[nodiscard]] auto NotifyManagedEntityDestroyed(EntityID entity_id, std::string_view context)
      -> bool;
  auto CaptureEntitySnapshot(EntityID entity_id, std::vector<std::byte>& out) -> bool;
  [[nodiscard]] auto RotateProxySession(EntityID entity_id, const SessionKey& session_key) -> bool;
  [[nodiscard]] auto TryCompleteLocalRelogin(PendingLogin pending) -> bool;
  void EnterDetachedGrace(EntityID entity_id);
  void ClearDetachedGrace(EntityID entity_id);
  [[nodiscard]] auto DeferredLoginCheckoutCount() const -> std::size_t;
  void CompletePrepareLoginFromCheckout(PendingLogin pending, DatabaseID dbid, uint16_t type_id,
                                        std::span<const std::byte> blob);
  void DeferPrepareLoginFromCheckout(EntityID blocking_entity_id, PendingLogin pending,
                                     DatabaseID dbid, uint16_t type_id,
                                     std::span<const std::byte> blob);
  void FailDeferredPrepareLogins(EntityID blocking_entity_id, std::string_view reason,
                                 std::vector<DatabaseID>* finished_dbids = nullptr);
  [[nodiscard]] auto ResumeDeferredPrepareLogins(EntityID blocking_entity_id) -> bool;
  void SubmitPrepareLogin(PendingLogin pending);
  void DispatchPrepareLogin(PendingLogin pending);
  void FinishLoginFlow(DatabaseID dbid);
  void StartDisconnectLogoff(EntityID entity_id);
  void FlushRemoteForceLogoffAcks(EntityID entity_id, bool success);
  void FlushAllRemoteForceLogoffAcks(bool success);
  void BeginLogoffPersist(EntityID entity_id, DatabaseID dbid, uint16_t type_id,
                          uint32_t continuation_request_id);
  void BeginForceLogoffPersist(uint32_t force_request_id, EntityID entity_id);
  void ContinueLoginAfterForceLogoff(uint32_t force_request_id);
  [[nodiscard]] auto FinalizeForceLogoff(EntityID entity_id) -> bool;
  void ProcessForceLogoffRequest(const baseapp::ForceLogoff& msg);
  auto ResolveInternalChannel(const Address& addr) -> Channel*;
  auto ResolveClientChannel(EntityID entity_id) -> Channel*;
  auto BindClient(EntityID entity_id, const Address& client_addr) -> bool;
  void UnbindClient(EntityID entity_id);
  void OnExternalClientDisconnect(Channel& ch);
  void SendPrepareLoginResult(const Address& reply_addr, const login::PrepareLoginResult& msg);
};

}  // namespace atlas

#endif  // ATLAS_SERVER_BASEAPP_BASEAPP_H_
