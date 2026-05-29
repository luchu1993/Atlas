#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_H_

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cellappmgr/cellappmgr_messages.h"  // cellappmgr::CellID
#include "cell_movement_system.h"
#include "coro/pending_rpc_registry.h"
#include "foundation/latency_histogram.h"
#include "math/vector3.h"
#include "network/address.h"
#include "network/reliable_udp.h"
#include "physics/physics_query.h"
#include "server/cellapp_peer_registry.h"
#include "server/entity_app.h"
#include "server/entity_types.h"
#include "server/id_client.h"

namespace atlas {

class Space;
class CellEntity;
class CellAppNativeProvider;
class Channel;
class Witness;

namespace physics {
class CollisionBackendFactory;
}

namespace cellapp {
struct CreateCellEntity;
struct DestroyCellEntity;
struct ClientCellRpcForward;
struct MovementCommandStartBroadcast;
struct MovementCommandEndBroadcast;
struct ClientMovementInputForward;
struct InternalCellRpc;
struct ClientRpcBroadcast;
struct CreateSpace;
struct DestroySpace;
struct AvatarUpdate;
struct EnableWitness;
struct DisableWitness;
struct SetAoIRadius;
struct CreateGhost;
struct DeleteGhost;
struct GhostPositionUpdate;
struct GhostDelta;
struct GhostSnapshotRefresh;
struct GhostSetReal;
struct GhostSetNextReal;
struct OffloadEntity;
struct OffloadEntityAck;
struct SpawnLocalEntity;
struct SpaceDataUpdate;
struct SpaceDataDelete;
struct SpaceDataSnapshotRequest;
struct SpaceDataSnapshot;
}  // namespace cellapp

namespace dbapp {
struct GetEntityIdsAck;
}

namespace machined {
struct BirthNotification;
struct DeathNotification;
}  // namespace machined

class CellApp : public EntityApp, public CellMovementHost {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  CellApp(EventDispatcher& dispatcher, NetworkInterface& network);
  ~CellApp() override;

  CellApp(const CellApp&) = delete;
  auto operator=(const CellApp&) -> CellApp& = delete;

  [[nodiscard]] auto Spaces() -> std::unordered_map<SpaceID, std::unique_ptr<Space>>& {
    return spaces_;
  }
  [[nodiscard]] auto FindEntity(EntityID cell_id) -> CellEntity*;
  [[nodiscard]] auto FindRealEntity(EntityID entity_id) -> CellEntity*;
  [[nodiscard]] auto FindSpace(SpaceID id) -> Space*;
  [[nodiscard]] auto NativeProvider() -> CellAppNativeProvider* { return native_provider_; }

  // Public so unit tests can drive the state machine directly without
  // a channel; signatures mirror RegisterTypedHandler's callback shape.
  void OnCreateCellEntity(const Address& src, Channel* ch, const cellapp::CreateCellEntity& msg);
  void OnDestroyCellEntity(const Address& src, Channel* ch, const cellapp::DestroyCellEntity& msg);
  void OnClientCellRpcForward(const Address& src, Channel* ch,
                              const cellapp::ClientCellRpcForward& msg);
  void OnClientMovementInputForward(const Address& src, Channel* ch,
                                    const cellapp::ClientMovementInputForward& msg);
  void OnInternalCellRpc(const Address& src, Channel* ch, const cellapp::InternalCellRpc& msg);
  void OnMovementCommandStartBroadcast(const Address& src, Channel* ch,
                                       const cellapp::MovementCommandStartBroadcast& msg);
  void OnMovementCommandEndBroadcast(const Address& src, Channel* ch,
                                     const cellapp::MovementCommandEndBroadcast& msg);
  void OnClientRpcBroadcast(const Address& src, Channel* ch,
                            const cellapp::ClientRpcBroadcast& msg);
  void OnCreateSpace(const Address& src, Channel* ch, const cellapp::CreateSpace& msg);
  void OnDestroySpace(const Address& src, Channel* ch, const cellapp::DestroySpace& msg);
  void OnAvatarUpdate(const Address& src, Channel* ch, const cellapp::AvatarUpdate& msg);
  void OnEnableWitness(const Address& src, Channel* ch, const cellapp::EnableWitness& msg);
  void OnDisableWitness(const Address& src, Channel* ch, const cellapp::DisableWitness& msg);
  void OnSetAoIRadius(const Address& src, Channel* ch, const cellapp::SetAoIRadius& msg);

  void OnCreateGhost(const Address& src, Channel* ch, const cellapp::CreateGhost& msg);
  void OnDeleteGhost(const Address& src, Channel* ch, const cellapp::DeleteGhost& msg);
  void OnGhostPositionUpdate(const Address& src, Channel* ch,
                             const cellapp::GhostPositionUpdate& msg);
  void OnGhostDelta(const Address& src, Channel* ch, const cellapp::GhostDelta& msg);
  void OnGhostSnapshotRefresh(const Address& src, Channel* ch,
                              const cellapp::GhostSnapshotRefresh& msg);
  void OnGhostSetReal(const Address& src, Channel* ch, const cellapp::GhostSetReal& msg);
  void OnGhostSetNextReal(const Address& src, Channel* ch, const cellapp::GhostSetNextReal& msg);
  void OnOffloadEntity(const Address& src, Channel* ch, const cellapp::OffloadEntity& msg);
  void OnOffloadEntityAck(const Address& src, Channel* ch, const cellapp::OffloadEntityAck& msg);

  void OnSpaceDataUpdate(const Address& src, Channel* ch, const cellapp::SpaceDataUpdate& msg);
  void OnSpaceDataDelete(const Address& src, Channel* ch, const cellapp::SpaceDataDelete& msg);
  void OnSpaceDataSnapshotRequest(const Address& src, Channel* ch,
                                  const cellapp::SpaceDataSnapshotRequest& msg);
  void OnSpaceDataSnapshot(const Address& src, Channel* ch, const cellapp::SpaceDataSnapshot& msg);

  // Owner-authoritative writes. The owner cellapp applies + fans out to
  // every peer holding the space; non-owners forward to the owner.
  void SetSpaceData(SpaceID space_id, uint16_t key_id, std::span<const uint8_t> value);
  void RemoveSpaceData(SpaceID space_id, uint16_t key_id);
  auto LoadCollisionAsset(SpaceID space_id, std::string_view path) -> bool;

  void OnGetEntityIdsAck(Channel& ch, const dbapp::GetEntityIdsAck& msg);

  // Mints an id from id_client_, places a cell-only CellEntity, restores C#.
  // Returns kInvalidEntityID when the id pool is empty or space_id is invalid.
  auto CreateLocalEntity(uint16_t type_id, SpaceID space_id, math::Vector3 pos, math::Vector3 dir,
                         bool on_ground) -> EntityID;

  // Refuses base-owned entities so base-side bookkeeping can't drift.
  void DestroyLocalEntity(EntityID entity_id);

  void OnAddCellToSpace(const Address& src, Channel* ch, const cellappmgr::AddCellToSpace& msg);
  void OnRemoveCellFromSpace(const Address& src, Channel* ch,
                             const cellappmgr::RemoveCellFromSpace& msg);
  void OnUpdateGeometry(const Address& src, Channel* ch, const cellappmgr::UpdateGeometry& msg);
  void OnShouldOffload(const Address& src, Channel* ch, const cellappmgr::ShouldOffload& msg);
  void OnRegisterCellAppAck(const Address& src, Channel* ch,
                            const cellappmgr::RegisterCellAppAck& msg);

  // Drops any mgr-control message arriving through a channel other than our
  // current CellAppMgr (a straggler from a dead mgr after takeover).
  [[nodiscard]] auto AcceptCellAppMgrMessage(Channel* ch, const char* tag) -> bool;

  [[nodiscard]] auto CellAppMgrStaleDrops() const -> uint64_t { return cellappmgr_stale_drops_; }

  // Non-zero after RegisterCellApp completes; included in load updates.
  [[nodiscard]] auto AppId() const -> uint32_t { return app_id_; }
  [[nodiscard]] auto CellAppMgrPidForTest() const -> uint32_t { return cellappmgr_pid_; }
  void SeedCellAppMgrSessionForTest(Channel* ch, uint32_t app_id, uint32_t pid) {
    cellappmgr_channel_ = ch;
    app_id_ = app_id;
    cellappmgr_pid_ = pid;
  }
  [[nodiscard]] auto ShouldReconnectCellAppMgrForBirthForTest(
      const machined::BirthNotification& n) const -> bool {
    return ShouldReconnectCellAppMgrForBirth(n);
  }
  void OnCellAppMgrDeathForTest(const machined::DeathNotification& n) {
    OnCellAppMgrDeath(n);
  }

  // Returns nullptr if the peer is unknown or already died.
  // Populated by the CellApp Birth/Death subscription in Init.
  [[nodiscard]] auto FindPeerChannel(const Address& addr) const -> Channel*;
  [[nodiscard]] auto IsTrustedCellAppPeer(const Address& addr) const -> bool;

  [[nodiscard]] auto GetRpcRegistry() -> PendingRpcRegistry& { return rpc_registry_; }

  // Test hook; the registry's Birth/Death subscription is the only
  // production writer.
  [[nodiscard]] auto PeerRegistryForTest() -> CellAppPeerRegistry& { return peer_registry_; }

  // Machined-driven; defers to HandlePeerLost.
  void OnPeerCellAppDeath(const Address& addr, Channel* dying, uint8_t reason);

  void OnOutboundChannelDeath(Channel& dying);

  // Single funnel for peer-cell death: address-keyed haunt + orphan-
  // ghost sweep, idempotent across the two death signals.
  void HandlePeerLost(const Address& peer_addr, bool normal);

  // Build but do not send; caller chooses transport.
  // Target Cell lets the receiver reject stale geometry.
  auto BuildOffloadMessage(const CellEntity& entity,
                           cellappmgr::CellID target_cell_id = 0) const
      -> cellapp::OffloadEntity;

  // EWMA of (work_time / expected_tick_period), refreshed every tick
  // at the start of OnTickComplete; consumed by SendInformCellLoad.
  [[nodiscard]] auto PersistentLoad() const -> float { return persistent_load_; }

  // Walks entity_population_ once per call; cheap at Atlas scales
  // (<= tens of thousands per cell).
  [[nodiscard]] auto NumRealEntities() const -> uint32_t;

  // Ghost-pump + offload-checker pass, called from OnEndOfTick. Public
  // so tests can step the tick pipeline deterministically.
  void TickGhostPump();
  void TickOffloadChecker();
  void TickMovementSystemForTest(float dt) { movement_system_.Tick(*this, dt); }

  // Auto-spawn the space-owner entity. Queues if EntityIDs aren't available
  // yet; OnGetEntityIdsAck drains the queue.
  void SpawnSpaceMaster(SpaceID space_id, const std::string& type_name);

  // Captures enough state to re-install a Real if Offload rejects or times out.
  struct PendingOffload {
    Address target_addr;
    TimePoint sent_at;
    SpaceID space_id{kInvalidSpaceID};
    cellappmgr::CellID cell_id{0};  // 0 => no local Cell membership to restore
    std::vector<Address> haunt_addrs;
    std::vector<std::byte> controller_blob;
    std::vector<std::byte> persistent_blob;
    uint16_t type_id{0};
    bool had_witness{false};
    float aoi_radius{0.f};
    float aoi_hysteresis{0.f};
    bool has_movement_state{false};
    movement::MovementState movement_state;
    std::vector<MovementPositionSample> movement_position_history;
    bool has_movement_command{false};
    movement::MovementCommand movement_command;
  };

  // No-op if the pending entry is already resolved.
  void RevertPendingOffload(EntityID entity_id, const char* reason);

  // Test hook; production writes happen in Offload send and resolution paths.
  [[nodiscard]] auto PendingOffloadsForTest() -> std::unordered_map<EntityID, PendingOffload>& {
    return pending_offloads_;
  }

  // Test hook for handlers that normally seed entity_population_.
  [[nodiscard]] auto EntityPopulationForTest() -> std::unordered_map<EntityID, CellEntity*>& {
    return entity_population_;
  }

 protected:
  [[nodiscard]] auto Init(int argc, char* argv[]) -> bool override;
  void Fini() override;

  void OnEndOfTick() override;
  void OnTickComplete() override;
  void RegisterWatchers() override;

  [[nodiscard]] auto CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> override;

 private:
  // RudpAddress() returns 0.0.0.0 when bound to any-interface; CellAppMgr
  // resolves it to 127.0.0.1 from inbound packet src, so use loopback here.
  [[nodiscard]] auto ResolveSelfAddr() -> Address;

  void TickControllers(float dt);
  void SetMovementIntent(EntityID entity_id, float dir_x, float dir_z, float speed_mps,
                         uint16_t buttons);
  auto SetMovementCommand(EntityID entity_id, const movement::MovementCommand& command) -> bool;
  auto ClearMovementCommand(EntityID entity_id, uint32_t command_id) -> bool;
  void TickWitnesses();
  auto RejectUntrustedCellAppPeer(const Address& src, const char* message_name) -> bool;

  // Cell-to-base state backup for entities with a live BaseAddr.
  // Sends BackupCellEntity every kBackupIntervalTicks.
  void TickBackupPump();

  // Owner-baseline pump for client-bound Real entities.
  // Gives reliable="false" properties a recovery channel.
  void TickClientBaselinePump();

  // Synchronous one-shot used by the pump and at AttachWitness time so
  // a fresh client sees owner-scope properties immediately.
  void SendOwnerBaselineFor(CellEntity& entity);

  // Wires the same send callbacks as OnEnableWitness.
  // Keeps BindClient, Offload arrival, and Offload revert aligned.
  void AttachWitness(CellEntity& entity, float aoi_radius, float hysteresis);

  // Shared spatial-insertion body for OnCreateCellEntity and CreateLocalEntity:
  // allocates the CellEntity, drops it into the space + owning local Cell.
  auto PlaceEntityInSpace(EntityID cell_id, uint16_t type_id, Space& space, math::Vector3 pos,
                          math::Vector3 dir, bool on_ground) -> CellEntity*;
  void AddRealToLocalCell(CellEntity& entity);
  [[nodiscard]] auto CapturePersistentBlob(const CellEntity& entity, const char* context) const
      -> std::vector<std::byte>;
  void RestoreScriptEntity(CellEntity& entity, uint16_t type_id,
                           std::span<const std::byte> script_init_data);

  // Shared teardown body for OnDestroyCellEntity and DestroyLocalEntity.
  void RemoveEntityFromSpace(CellEntity* entity);
  [[nodiscard]] auto FindMovementActor(EntityID entity_id,
                                       MovementActorSnapshot& out) -> bool override;
  [[nodiscard]] auto MovementNow() -> TimePoint override;
  [[nodiscard]] auto MovementServerTick() const -> uint32_t override;
  void PublishMovementState(EntityID entity_id,
                            const movement::MovementState& state) override;
  void SendMovementStateAck(EntityID entity_id, const movement::MovementState& state,
                            uint32_t server_tick) override;
  void SendMovementCommandStart(EntityID entity_id,
                                const movement::MovementCommand& command) override;
  void SendMovementCommandStartToBaseApps(
      EntityID source_entity_id, uint32_t cell_epoch,
      const movement::MovementCommand& command,
      std::unordered_map<Address, std::vector<EntityID>>& by_baseapp);
  void SendMovementCommandEnd(EntityID entity_id, uint32_t command_id,
                              const movement::MovementState& state,
                              uint32_t server_tick,
                              movement::MovementCommandEndReason reason) override;
  void SendMovementCommandEndToBaseApps(
      EntityID source_entity_id, uint32_t cell_epoch, uint32_t command_id,
      uint32_t server_tick, movement::MovementCommandEndReason reason,
      const movement::MovementState& state,
      std::unordered_map<Address, std::vector<EntityID>>& by_baseapp);
  auto RestoreMovementState(EntityID entity_id, const movement::MovementState& state) -> bool;
  void RestoreMovementPositionHistoryFromOffload(
      EntityID entity_id, std::span<const MovementPositionSample> samples);
  void RestoreMovementPositionHistoryAsIs(EntityID entity_id,
                                          std::span<const MovementPositionSample> samples);
  auto RestoreMovementCommand(EntityID entity_id,
                              const movement::MovementCommand& command) -> bool;

  // EWMA update from LastTickWorkDuration() / ExpectedTickPeriod();
  // called every tick from OnTickComplete.
  void UpdatePersistentLoad();

  // Refill water-level pool from DBApp when running low; called per tick.
  void MaybeRequestMoreIds();

  // Sends cellappmgr::InformCellLoad. No-op if not yet registered
  // (cellappmgr_channel_ null or app_id_ == 0).
  void SendInformCellLoad();
  // Reports the full BSP geometry this CellApp holds per space so a freshly
  // (re)started CellAppMgr can rebuild its partitions from live workers.
  void SendRecoverCellAppState();
  [[nodiscard]] auto ShouldReconnectCellAppMgrForBirth(
      const machined::BirthNotification& n) const -> bool;
  void OnCellAppMgrBirth(const machined::BirthNotification& n);
  void OnCellAppMgrDeath(const machined::DeathNotification& n);
  void ClearCellAppMgrSession();
  [[nodiscard]] auto FindLocalCellIdFor(const CellEntity& entity) const -> cellappmgr::CellID;
  void RecordScriptTick(uint32_t entity_id, uint64_t elapsed_us);
  void RecordNativeTick(EntityID entity_id, cellappmgr::CellID cell_id, Duration elapsed);
  void RecordNativeTick(const CellEntity& entity, Duration elapsed);

  // SpaceData routing helpers. `exclude` lets a forwarded write skip
  // bouncing back to its sender; owner = cellapp holding the BSP primary cell.
  [[nodiscard]] auto FindSpaceOwnerChannel(const Space& space) -> Channel*;
  void RequestSpaceDataSnapshot(Space& space, const Address& source_addr);
  void SendAddCellToSpaceAck(Channel* ch, SpaceID space_id, cellappmgr::CellID cell_id);
  void BroadcastSpaceDataUpdate(const Space& space, uint16_t key_id,
                                std::span<const uint8_t> value, const Address* exclude);
  void BroadcastSpaceDataDelete(const Space& space, uint16_t key_id, const Address* exclude);

  // Fan-out to every witness in the space; called after each SpaceData
  // mutation so client observers receive the change outside the entity AoI path.
  void PushSpaceDataUpdateToWitnesses(Space& space, uint16_t key_id,
                                      std::span<const uint8_t> value);
  void PushSpaceDataDeleteToWitnesses(Space& space, uint16_t key_id);
  void PushSpaceDataInitToWitnesses(Space& space);

  // Constructs a Space with the process collision backend already injected.
  [[nodiscard]] auto MakeSpace(SpaceID id) -> std::unique_ptr<Space>;

  std::shared_ptr<const physics::CollisionBackendFactory> collision_backend_factory_;
  std::unordered_map<SpaceID, std::unique_ptr<Space>> spaces_;
  struct PendingAddCellAck {
    SpaceID space_id{kInvalidSpaceID};
    cellappmgr::CellID cell_id{0};
    Channel* channel{nullptr};
  };
  std::vector<PendingAddCellAck> pending_primary_handoff_acks_;

  // Non-owning; the owning unique_ptr lives in the peer Space's entities_ map.
  // Holds both Real and Ghost entities.
  std::unordered_map<EntityID, CellEntity*> entity_population_;

  // Backup cadence is tight because BaseApp only sees backup bytes.
  // DB writes also need reasonably fresh cell-side snapshots.
  static constexpr uint32_t kBackupIntervalTicks = 50;
  uint32_t backup_tick_counter_{0};
  uint32_t ghost_persistent_tick_counter_{0};

  // Baseline is a bandwidth-insensitive safety net for
  // reliable="false" attributes; tighter than backup is unnecessary.
  static constexpr uint32_t kClientBaselineIntervalTicks = 120;
  uint32_t client_baseline_tick_counter_{0};

  // Assigned by CellAppMgr's RegisterCellAppAck. 0 => not yet
  // registered; SendInformCellLoad short-circuits while 0.
  uint32_t app_id_{0};
  uint32_t cellappmgr_pid_{0};
  Channel* cellappmgr_channel_{nullptr};
  // Count of mgr-control messages dropped for arriving on a channel that is
  // not our current CellAppMgr (stale mgr after a takeover).
  uint64_t cellappmgr_stale_drops_{0};
  Channel* dbapp_channel_{nullptr};
  IDClient id_client_;

  // EWMA-smoothed load factor in [0, 1+] - the number CellAppMgr's
  // BSP balancer consumes.
  float persistent_load_{0.f};

  // Throttle steady-state load reports unless load/count changes or
  // heartbeat elapses, keeping manager bandwidth bounded.
  float last_sent_load_{-1.f};
  uint32_t last_sent_entity_count_{UINT32_MAX};
  TimePoint last_sent_load_time_{};
  TimePoint cell_load_counter_window_start_{Clock::now()};
  uint64_t inform_cell_load_send_failures_{0};
  static constexpr float kInformCellLoadDelta = 0.01f;
  static constexpr Duration kInformCellLoadHeartbeat = std::chrono::seconds(1);

  // 30-sample ring (~30s @ 1Hz) damps the per-cell median against single-
  // entity jitter that would otherwise drag the split position to one coord.
  struct CellMedianWindow {
    static constexpr uint8_t kCap = 30;
    std::array<float, kCap> xs{};
    std::array<float, kCap> zs{};
    uint8_t count{0};
    uint8_t head{0};
    void Push(float x, float z) {
      xs[head] = x;
      zs[head] = z;
      head = static_cast<uint8_t>((head + 1) % kCap);
      if (count < kCap) ++count;
    }
    [[nodiscard]] auto Mean() const -> std::pair<float, float> {
      if (count == 0) return {0.f, 0.f};
      float sx = 0.f, sz = 0.f;
      for (uint8_t i = 0; i < count; ++i) { sx += xs[i]; sz += zs[i]; }
      return {sx / count, sz / count};
    }
  };
  std::unordered_map<cellappmgr::CellID, CellMedianWindow> cell_median_window_;

  struct CellLoadCounters {
    uint64_t script_tick_us{0};
    uint64_t native_tick_us{0};
    uint64_t aoi_reliable_bytes{0};
    uint64_t aoi_unreliable_bytes{0};
    uint64_t backup_bytes{0};
  };
  std::unordered_map<cellappmgr::CellID, CellLoadCounters> cell_load_counters_;
  struct EntityLoadCounters {
    uint64_t script_tick_us{0};
    uint64_t native_tick_us{0};
  };
  std::unordered_map<EntityID, EntityLoadCounters> entity_load_counters_;

  // Shared registry (atlas_server) so both BaseApp and CellApp route
  // through the same Birth/Death + self-filter code.
  CellAppPeerRegistry peer_registry_;

  // Inbound ClientCellRpcForward must come from this trusted set.
  // Otherwise the sender could bypass BaseApp validation.
  std::unordered_set<Address> trusted_baseapps_;

  // Per-source spoofing audit; source != target on own-client paths
  // means desync or forgery. Aggregated by source entity.
  std::unordered_map<EntityID, uint64_t> caller_spoof_violations_;
  uint64_t caller_spoof_violations_total_{0};
  uint64_t untrusted_cellapp_messages_total_{0};
  uint64_t create_cell_entity_total_{0};
  uint64_t create_cell_entity_restore_payload_total_{0};
  uint64_t create_cell_entity_restore_ghost_backup_total_{0};
  uint64_t create_cell_entity_restore_empty_total_{0};
  uint64_t create_cell_entity_failures_total_{0};
  uint64_t create_cell_entity_death_restore_total_{0};
  uint64_t create_cell_entity_death_restore_payload_total_{0};
  uint64_t create_cell_entity_death_restore_ghost_backup_total_{0};
  uint64_t create_cell_entity_death_restore_empty_total_{0};
  uint64_t create_cell_entity_death_restore_failures_total_{0};
  uint64_t create_cell_entity_death_restore_promoted_total_{0};
  uint64_t ghost_promoted_to_real_total_{0};
  CellMovementSystem movement_system_;

 public:
  // Test-only - production callers don't touch this; the machined
  // Subscribe callback in Init is the only writer.
  void InsertTrustedBaseAppForTest(const Address& addr) { trusted_baseapps_.insert(addr); }

  [[nodiscard]] auto MovementSystemForTest() -> CellMovementSystem& { return movement_system_; }
  [[nodiscard]] auto MovementPhysicsQueryForTest(SpaceID space_id)
      -> physics::StaticPhysicsQuery*;

  // Test-only - forces an immediate load report; production cadence is the
  // periodic OnTickComplete timer.
  void FlushLoadReportForTest() {
    last_sent_load_time_ = {};
    SendInformCellLoad();
  }

  // Test-only - production code receives IDs via DBApp's GetEntityIdsAck.
  [[nodiscard]] auto GetIdClientForTest() const -> const IDClient& { return id_client_; }

  // Test-only - production callers go through ScriptApp::Init; tests need
  // direct access to wire fake native callbacks before invoking handlers.
  [[nodiscard]] auto CreateNativeProviderForTest() -> std::unique_ptr<INativeApiProvider> {
    return CreateNativeProvider();
  }

 private:
  std::unordered_map<EntityID, PendingOffload> pending_offloads_;

  PendingRpcRegistry rpc_registry_;

  // Space masters whose primary-cell add arrived before this CellApp had any
  // EntityIDs allocated; drained on the next GetEntityIdsAck. (space_id, type).
  std::vector<std::pair<SpaceID, std::string>> pending_space_master_spawns_;

  // Per-tick scratch for demand-based witness budget allocation.
  // Cleared, not deallocated, so steady-state ticks allocate 0.
  struct ObserverDemand {
    Witness* w;
    uint32_t want;
  };
  std::vector<ObserverDemand> witness_demand_scratch_;

  // Called each tick from OnEndOfTick.
  void TickOffloadAckTimeouts();

  static constexpr Duration kOffloadAckTimeout = std::chrono::seconds(5);

  // Reject AvatarUpdate displacement beyond 50 m/tick (~500 m/s at
  // 10 Hz - well above any realistic player speed).
  static constexpr float kMaxSingleTickMove = 50.f;

  // Concrete-typed alias so handlers can reach CellApp-specific state
  // without a dynamic_cast; ScriptApp owns the unique_ptr.
  CellAppNativeProvider* native_provider_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_H_
