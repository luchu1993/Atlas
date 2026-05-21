#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_H_

#include <memory>
#include <span>
#include <unordered_map>
#include <unordered_set>

#include "cellappmgr/cellappmgr_messages.h"  // cellappmgr::CellID
#include "coro/pending_rpc_registry.h"
#include "math/vector3.h"
#include "network/address.h"
#include "network/reliable_udp.h"
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

namespace cellapp {
struct CreateCellEntity;
struct DestroyCellEntity;
struct ClientCellRpcForward;
struct InternalCellRpc;
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

// Spatial-simulation server process. Inherits from EntityApp (peer of
// BaseApp) and owns the local Space map plus a single entity index
// keyed by the unified entity id allocated by DBApp.
// Tick flow:
//   OnStartOfTick - drives EntityApp's C# on_tick
//   ... Updatables ...
//   C# on_tick -> publish_replication_frame via NativeApi
//   OnTickComplete - drives TickControllers(dt) then TickWitnesses()
//   OnEndOfTick    - ghost pump + offload checker + ack timeouts
class CellApp : public EntityApp {
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
  void OnInternalCellRpc(const Address& src, Channel* ch, const cellapp::InternalCellRpc& msg);
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

  void OnGetEntityIdsAck(Channel& ch, const dbapp::GetEntityIdsAck& msg);

  // Mints an id from id_client_, places a cell-only CellEntity, restores C#.
  // Returns kInvalidEntityID when the id pool is empty or space_id is invalid.
  auto CreateLocalEntity(uint16_t type_id, SpaceID space_id, math::Vector3 pos, math::Vector3 dir,
                         bool on_ground) -> EntityID;

  // Refuses base-owned entities so base-side bookkeeping can't drift.
  void DestroyLocalEntity(EntityID entity_id);

  void OnAddCellToSpace(const Address& src, Channel* ch, const cellappmgr::AddCellToSpace& msg);
  void OnUpdateGeometry(const Address& src, Channel* ch, const cellappmgr::UpdateGeometry& msg);
  void OnShouldOffload(const Address& src, Channel* ch, const cellappmgr::ShouldOffload& msg);
  void OnRegisterCellAppAck(const Address& src, Channel* ch,
                            const cellappmgr::RegisterCellAppAck& msg);

  // Non-zero after RegisterCellApp completes; reported back to
  // CellAppMgr in load updates so the manager can attribute traffic to
  // the right CellApp instance.
  [[nodiscard]] auto AppId() const -> uint32_t { return app_id_; }

  // Returns nullptr if the peer isn't known (not yet connected or
  // already died). Populated by the machined ProcessType::kCellApp
  // Birth/Death subscription in Init.
  [[nodiscard]] auto FindPeerChannel(const Address& addr) const -> Channel*;

  [[nodiscard]] auto GetRpcRegistry() -> PendingRpcRegistry& { return rpc_registry_; }

  // Test hook; the registry's Birth/Death subscription is the only
  // production writer.
  [[nodiscard]] auto PeerRegistryForTest() -> CellAppPeerRegistry& { return peer_registry_; }

  // Invoked from the peer-registry death handler. Drops Ghosts whose
  // Real lived on the dying peer (their authoritative source is gone)
  // and removes the dying Channel* from every Real's Haunt list so
  // later broadcasts don't chase a freed pointer.
  void OnPeerCellAppDeath(const Address& addr, Channel* dying);

  void OnOutboundChannelDeath(Channel& dying);

  // Build but don't send - caller chooses transport. The C#
  // SerializeEntity callback fills persistent_blob when registered;
  // the receiver rebuilds local Cell membership from the arriving
  // position so no target_cell_id needs to travel on the wire.
  auto BuildOffloadMessage(const CellEntity& entity) const -> cellapp::OffloadEntity;

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

  // Auto-spawn the space-owner entity. Queues if EntityIDs aren't available
  // yet; OnGetEntityIdsAck drains the queue.
  void SpawnSpaceMaster(SpaceID space_id, const std::string& type_name);

  // Captures everything needed to re-install a Real locally if the
  // Offload receiver rejects (or never acks). Inserted by
  // TickOffloadChecker right before ConvertRealToGhost; removed by
  // OnOffloadEntityAck (success) or by the failure-revert path.
  // BaseApp never heard the move (CurrentCell is sent by the receiver
  // on success, not the sender on send) so a revert leaves client RPC
  // routing pointing at us.
  struct PendingOffload {
    Address target_addr;
    TimePoint sent_at;
    SpaceID space_id{kInvalidSpaceID};
    cellappmgr::CellID cell_id{0};  // 0 => no local Cell membership to restore
    std::vector<Address> haunt_addrs;
    std::vector<std::byte> controller_blob;
    // Persistent state captured pre-ConvertRealToGhost so revert can
    // recreate the C# instance via restore_entity_fn after script teardown.
    std::vector<std::byte> persistent_blob;
    uint16_t type_id{0};
    // Captured pre-ConvertRealToGhost so revert reattaches with the
    // script-authored radius / hysteresis intact.
    bool had_witness{false};
    float aoi_radius{0.f};
    float aoi_hysteresis{0.f};
  };

  // No-op if the entry is already resolved. Callers: the
  // OnOffloadEntityAck failure branch, TickOffloadAckTimeouts, and
  // tests that seed via PendingOffloadsForTest.
  void RevertPendingOffload(EntityID entity_id, const char* reason);

  // Test hook - TickOffloadChecker (insert) and the Ack / timeout
  // paths (erase) are the only production writers.
  [[nodiscard]] auto PendingOffloadsForTest() -> std::unordered_map<EntityID, PendingOffload>& {
    return pending_offloads_;
  }

  // Test hook - production writers are OnCreateCellEntity /
  // OnOffloadEntity / OnCreateGhost. Tests that bypass those handlers
  // seed lookups here for RevertPendingOffload and the RPC dispatch
  // paths.
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
  void TickWitnesses();

  // Cell->base state backup. Fires every kBackupIntervalTicks for every
  // entity with a live BaseAddr, capturing the cell-side Serialize
  // output and shipping BackupCellEntity (msg 2018) to the BaseApp.
  void TickBackupPump();

  // Owner-baseline pump. Every kClientBaselineIntervalTicks every
  // client-bound Real produces its owner-scope snapshot via
  // get_owner_snapshot and ships ReplicatedBaselineFromCell to the
  // client. Gives reliable="false" properties a recovery channel.
  void TickClientBaselinePump();

  // Synchronous one-shot used by the pump and at AttachWitness time so
  // a fresh client sees owner-scope properties immediately.
  void SendOwnerBaselineFor(CellEntity& entity);

  // Wires the same Reliable / Unreliable send callbacks that
  // OnEnableWitness uses, so the pipeline is identical whether the
  // witness came up via BindClient, Offload arrival, or Offload revert.
  void AttachWitness(CellEntity& entity, float aoi_radius, float hysteresis);

  // Shared spatial-insertion body for OnCreateCellEntity and CreateLocalEntity:
  // allocates the CellEntity, drops it into the space + owning local Cell.
  auto PlaceEntityInSpace(EntityID cell_id, uint16_t type_id, Space& space, math::Vector3 pos,
                          math::Vector3 dir, bool on_ground) -> CellEntity*;

  // Shared teardown body for OnDestroyCellEntity and DestroyLocalEntity.
  void RemoveEntityFromSpace(CellEntity* entity);

  // EWMA update from LastTickWorkDuration() / ExpectedTickPeriod();
  // called every tick from OnTickComplete.
  void UpdatePersistentLoad();

  // Refill water-level pool from DBApp when running low; called per tick.
  void MaybeRequestMoreIds();

  // Sends cellappmgr::InformCellLoad. No-op if not yet registered
  // (cellappmgr_channel_ null or app_id_ == 0).
  void SendInformCellLoad();

  // SpaceData routing helpers. `exclude` lets a forwarded write skip
  // bouncing back to its sender; owner = cellapp holding the BSP primary cell.
  [[nodiscard]] auto FindSpaceOwnerChannel(const Space& space) -> Channel*;
  void BroadcastSpaceDataUpdate(const Space& space, uint16_t key_id,
                                std::span<const uint8_t> value, const Address* exclude);
  void BroadcastSpaceDataDelete(const Space& space, uint16_t key_id, const Address* exclude);

  // Fan-out to every witness in the space — called after each SpaceData
  // mutation so client observers receive the change outside the entity AoI path.
  void PushSpaceDataUpdateToWitnesses(Space& space, uint16_t key_id,
                                      std::span<const uint8_t> value);
  void PushSpaceDataDeleteToWitnesses(Space& space, uint16_t key_id);
  void PushSpaceDataInitToWitnesses(Space& space);

  std::unordered_map<SpaceID, std::unique_ptr<Space>> spaces_;

  // Non-owning - the owning unique_ptr lives in the peer Space's
  // entities_ map. Holds both Real and Ghost entities; FindRealEntity
  // gates on IsReal() to keep client RPC routing off Ghost entries.
  std::unordered_map<EntityID, CellEntity*> entity_population_;

  // Tight cadence (50 ticks ~ 1 s at 50 Hz) because backup bytes are
  // the only authoritative cell-side state BaseApp sees, and the DB
  // write path wants reasonably fresh snapshots.
  static constexpr uint32_t kBackupIntervalTicks = 50;
  uint32_t backup_tick_counter_{0};

  // Baseline is a bandwidth-insensitive safety net for
  // reliable="false" attributes; tighter than backup is unnecessary.
  static constexpr uint32_t kClientBaselineIntervalTicks = 120;
  uint32_t client_baseline_tick_counter_{0};

  // Assigned by CellAppMgr's RegisterCellAppAck. 0 => not yet
  // registered; SendInformCellLoad short-circuits while 0.
  uint32_t app_id_{0};
  Channel* cellappmgr_channel_{nullptr};
  Channel* dbapp_channel_{nullptr};
  IDClient id_client_;

  // EWMA-smoothed load factor in [0, 1+] - the number CellAppMgr's
  // BSP balancer consumes.
  float persistent_load_{0.f};

  // SendInformCellLoad skips the wire hop when neither value has
  // shifted meaningfully AND a heartbeat interval hasn't elapsed, so
  // a steady-state CellApp doesn't burn tick-rate bandwidth on the
  // manager just to say "still nothing changed."
  float last_sent_load_{-1.f};
  uint32_t last_sent_entity_count_{UINT32_MAX};
  TimePoint last_sent_load_time_{};
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

  // Shared registry (atlas_server) so both BaseApp and CellApp route
  // through the same Birth/Death + self-filter code.
  CellAppPeerRegistry peer_registry_;

  // Inbound ClientCellRpcForward whose wire src isn't in this set is
  // dropped - an unregistered sender forging the message would bypass
  // BaseApp's L1/L2 validation.
  std::unordered_set<Address> trusted_baseapps_;

  // Per-source caller-spoofing audit: BaseApp must stamp source_entity_id
  // from the authenticated channel binding, so source != target on an
  // OWN_CLIENT method indicates either a desync or a forged/exploited
  // path. Aggregated by source so a single misbehaving client surfaces
  // even when target jitters across nearby entities.
  std::unordered_map<EntityID, uint64_t> caller_spoof_violations_;
  uint64_t caller_spoof_violations_total_{0};

 public:
  // Test-only - production callers don't touch this; the machined
  // Subscribe callback in Init is the only writer.
  void InsertTrustedBaseAppForTest(const Address& addr) { trusted_baseapps_.insert(addr); }

  // Test-only — production code receives IDs via DBApp's GetEntityIdsAck.
  [[nodiscard]] auto GetIdClientForTest() const -> const IDClient& { return id_client_; }

  // Test-only — production callers go through ScriptApp::Init; tests need
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
  // Cleared (not deallocated) at the start of every TickWitnesses
  // sweep so steady-state ticks allocate 0.
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
