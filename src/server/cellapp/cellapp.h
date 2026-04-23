#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_H_

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "cellappmgr/cellappmgr_messages.h"  // cellappmgr::CellID
#include "network/address.h"
#include "server/cellapp_peer_registry.h"
#include "server/entity_app.h"
#include "server/entity_types.h"

namespace atlas {

class Space;
class CellEntity;
class CellAppNativeProvider;
class Channel;

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
}  // namespace cellapp

// cellappmgr struct types are fully declared via cellappmgr_messages.h
// above — kept included rather than forward-declared so CellID and
// message-struct fields are visible in inline helpers on this header.

// CellApp — spatial-simulation server process.
//
// Inherits from EntityApp (same level as BaseApp) and wires:
//   • Space map — the local set of spatial partitions this cell serves
//   • Dual entity index (cell_id → *, base_id → *) — RPC / AoI route on
//     base_id while internal management uses the cell-local id
//   • CellAppNativeProvider — C# ↔ C++ interop anchored to this CellApp's
//     entity lookup
//   • Message handlers for the CellApp inbound messages (IDs 3000-3099)
//
// Tick flow:
//   OnStartOfTick  — drives EntityApp's C# on_tick
//   … Updatables …
//   C# on_tick → publish_replication_frame via NativeApi
//   OnTickComplete — drives TickControllers(dt) then TickWitnesses()
//   OnEndOfTick    — ghost pump + offload checker + ack timeouts

class CellApp : public EntityApp {
 public:
  static auto Run(int argc, char* argv[]) -> int;

  CellApp(EventDispatcher& dispatcher, NetworkInterface& network);
  ~CellApp() override;

  CellApp(const CellApp&) = delete;
  auto operator=(const CellApp&) -> CellApp& = delete;

  // ---- Spatial indices (exposed for tests) --------------------------------

  [[nodiscard]] auto Spaces() -> std::unordered_map<SpaceID, std::unique_ptr<Space>>& {
    return spaces_;
  }
  [[nodiscard]] auto FindEntity(EntityID cell_id) -> CellEntity*;
  [[nodiscard]] auto FindEntityByBaseId(EntityID base_id) -> CellEntity*;
  [[nodiscard]] auto FindSpace(SpaceID id) -> Space*;
  [[nodiscard]] auto NativeProvider() -> CellAppNativeProvider* { return native_provider_; }

  // ---- Public handlers (friended-by-test access) --------------------------
  //
  // These are normally called by the network dispatch table; exposing them
  // as public lets unit tests drive the state machine without spinning up
  // a channel. Signatures mirror the RegisterTypedHandler callback shape.

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

  // ---- Inter-CellApp handlers ----
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

  // ---- CellAppMgr → CellApp handlers ----
  void OnAddCellToSpace(const Address& src, Channel* ch, const cellappmgr::AddCellToSpace& msg);
  void OnUpdateGeometry(const Address& src, Channel* ch, const cellappmgr::UpdateGeometry& msg);
  void OnShouldOffload(const Address& src, Channel* ch, const cellappmgr::ShouldOffload& msg);
  void OnRegisterCellAppAck(const Address& src, Channel* ch,
                            const cellappmgr::RegisterCellAppAck& msg);

  // ---- app_id / EntityID bookkeeping --------
  //
  // After RegisterCellApp succeeds, AppId() holds the non-zero value
  // assigned by CellAppMgr. AllocateCellEntityId packs it into the
  // high 8 bits so EntityIDs are unique cluster-wide.
  [[nodiscard]] auto AppId() const -> uint32_t { return app_id_; }

  // ---- Peer CellApp routing (exposed for tests) --------------------------
  //
  // Lookup Channel* for a peer CellApp by address. Populated via the
  // machined ProcessType::kCellApp Birth/Death subscription in Init.
  // Returns nullptr if the peer isn't known (either not yet connected or
  // already died).
  [[nodiscard]] auto FindPeerChannel(const Address& addr) const -> Channel*;

  // Test hook — lets scenarios seed peer channels without spinning up
  // machined. Production writers are the registry's Birth/Death
  // subscription in Init; nothing else mutates this.
  [[nodiscard]] auto PeerRegistryForTest() -> CellAppPeerRegistry& { return peer_registry_; }

  // Sweep application-level references to a dying peer CellApp: Ghosts
  // whose Real lived on that peer are dropped (their authoritative
  // source is gone), and every Real's Haunt list drops the dying
  // Channel* so later broadcasts don't chase a freed pointer.
  // Invoked from the peer-registry death handler; exposed for tests.
  void OnPeerCellAppDeath(const Address& addr, Channel* dying);

  // ---- Offload orchestration (exposed for tests) --------------------------

  // Build an OffloadEntity message for `entity`. Does NOT send; caller
  // decides transport. The C# SerializeEntity callback fills
  // `persistent_blob` when registered. The receiver rebuilds its local
  // Cell membership by querying its own BSP with the arriving position,
  // so no target_cell_id needs to travel on the wire.
  auto BuildOffloadMessage(const CellEntity& entity) const -> cellapp::OffloadEntity;

  // Per-tick load estimate — EWMA of (work_time / expected_tick_period).
  // Updated at the start of OnTickComplete so every tick feeds the
  // smoother; the value feeds SendInformCellLoad.
  [[nodiscard]] auto PersistentLoad() const -> float { return persistent_load_; }

  // Count of Real entities currently hosted on this CellApp. Walks
  // entity_population_ once per call — cheap at Atlas's entity scales
  // (≤ tens of thousands per cell).
  [[nodiscard]] auto NumRealEntities() const -> uint32_t;

  // Ghost-pump + offload-checker pass, called from OnEndOfTick. Exposed
  // so unit tests can step the tick pipeline deterministically.
  void TickGhostPump();
  void TickOffloadChecker();

  // ---- Offload rollback ---------------------
  //
  // PendingOffload captures everything the CellApp needs to re-install
  // a Real locally when the receiver rejects (or never acks) an
  // OffloadEntity. Inserted by TickOffloadChecker right before
  // ConvertRealToGhost; removed by OnOffloadEntityAck (success) or by
  // the failure-revert path.
  //
  // Revert: ConvertGhostToReal (preserves replication_state_), rehydrate
  // haunts + local-Cell membership + controllers (from controller_blob).
  // BaseApp never heard we moved — CurrentCell is sent by the receiver
  // on success, not the sender on send — so client RPC routing stays
  // correct across a revert.
  //
  // Timeout: entries older than kOffloadAckTimeout are reverted each
  // tick via TickOffloadAckTimeouts, on the assumption the receiver is
  // unreachable.
  struct PendingOffload {
    Address target_addr;
    TimePoint sent_at;
    SpaceID space_id{kInvalidSpaceID};
    cellappmgr::CellID cell_id{0};  // 0 ⇒ no local Cell membership to restore
    std::vector<Address> haunt_addrs;
    std::vector<std::byte> controller_blob;
    // Witness state captured pre-ConvertRealToGhost so revert can
    // reattach with the script-authored radius / hysteresis intact.
    bool had_witness{false};
    float aoi_radius{0.f};
    float aoi_hysteresis{0.f};
  };

  // Revert a pending Offload back to a live Real. No-op if the entry is
  // already resolved. Callers: OnOffloadEntityAck failure branch,
  // TickOffloadAckTimeouts, and unit tests that seed a pending entry
  // via PendingOffloadsForTest().
  void RevertPendingOffload(EntityID entity_id, const char* reason);

  // Test hook — direct mutable access to the pending map. Production
  // writers are TickOffloadChecker (insert) + the Ack / timeout paths
  // (erase).
  [[nodiscard]] auto PendingOffloadsForTest() -> std::unordered_map<EntityID, PendingOffload>& {
    return pending_offloads_;
  }

  // Test hook — direct mutable access to entity_population_. Production
  // writers are OnCreateCellEntity / OnOffloadEntity / OnCreateGhost.
  // Unit tests that bypass those handlers (to avoid spinning up the
  // full network+CLR pipeline) use this to seed lookups consumed by
  // RevertPendingOffload and the RPC dispatch paths.
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
  // Internal helpers.
  void TickControllers(float dt);
  void TickWitnesses();
  // Cell→base state backup. Fires every kBackupIntervalTicks ticks for
  // every entity with a live BaseAddr, capturing the cell-side
  // Serialize output as opaque bytes and shipping BackupCellEntity
  // (msg 2018) to the BaseApp.
  void TickBackupPump();

  // CellApp-side owner baseline pump. Every kClientBaselineIntervalTicks
  // ticks, every Real entity with a witness (client-bound) produces its
  // owner-scope snapshot via get_owner_snapshot and ships it through
  // BaseApp as ReplicatedBaselineFromCell (msg 2019) →
  // ReplicatedBaselineToClient (0xF002). Gives reliable="false"
  // properties a genuine recovery channel.
  void TickClientBaselinePump();

  // Attach a Witness to the given Real entity with the specified radius
  // + hysteresis. Wires the same Reliable / Unreliable send callbacks
  // that OnEnableWitness uses so the pipeline is identical whether the
  // witness came up via BindClient, Offload arrival, or Offload revert.
  void AttachWitness(CellEntity& entity, float aoi_radius, float hysteresis);

  // EWMA update of persistent_load_. Reads LastTickWorkDuration() +
  // ExpectedTickPeriod() from the ServerApp base class. Called every
  // tick from OnTickComplete.
  void UpdatePersistentLoad();

  // Dispatch cellappmgr::InformCellLoad to the CellAppMgr channel with
  // current persistent_load_ + NumRealEntities(). No-op if not yet
  // registered (cellappmgr_channel_ is null or app_id_ is 0). Called
  // every tick from OnTickComplete.
  void SendInformCellLoad();

  [[nodiscard]] auto AllocateCellEntityId() -> EntityID;

  std::unordered_map<SpaceID, std::unique_ptr<Space>> spaces_;

  // cell_entity_id → CellEntity*. Non-owning — the owning unique_ptr lives
  // in the peer Space's entities_ map. Holds BOTH Real and Ghost entities
  // (EntityIDs are cluster-wide so no collision risk).
  std::unordered_map<EntityID, CellEntity*> entity_population_;

  // base_entity_id → CellEntity*. Required for RPC routing because
  // clients and BaseApp both identify entities by their base_entity_id,
  // which is stable across CellApp offloads. Only Real entities appear
  // here — client RPCs never dispatch to a Ghost.
  std::unordered_map<EntityID, CellEntity*> base_entity_population_;

  EntityID next_entity_id_{1};

  // Backup pump cadence. 50 ticks ≈ 1 s at 50 Hz. Tight cadence because
  // backup bytes are the only authoritative cell-side state BaseApp
  // sees, and the DB write path wants reasonably fresh snapshots.
  static constexpr uint32_t kBackupIntervalTicks = 50;
  uint32_t backup_tick_counter_{0};

  // Client baseline pump cadence. Baseline is a bandwidth-insensitive
  // safety net for reliable="false" attributes; tighter than backup is
  // unnecessary.
  static constexpr uint32_t kClientBaselineIntervalTicks = 120;
  uint32_t client_baseline_tick_counter_{0};

  // app_id assigned by CellAppMgr's RegisterCellAppAck. Forms the high
  // 8 bits of every EntityID we mint. 0 ⇒ not yet registered; IDs
  // allocated before registration use high byte 0 and will clash once
  // a CellAppMgr is in the cluster, so callers should avoid creating
  // entities pre-registration in production. Unit tests never observe
  // app_id and continue to work with app_id_ == 0.
  uint32_t app_id_{0};
  Channel* cellappmgr_channel_{nullptr};

  // EWMA-smoothed load factor in [0, 1+] — the number CellAppMgr's BSP
  // balancer consumes. Updated every tick by UpdatePersistentLoad.
  float persistent_load_{0.f};

  // Last InformCellLoad payload and dispatch time. SendInformCellLoad
  // skips the wire hop when neither value has shifted meaningfully
  // since the last send AND a heartbeat interval hasn't elapsed, so a
  // steady-state CellApp doesn't burn tick-rate bandwidth on the
  // manager just to say "still nothing changed."
  float last_sent_load_{-1.f};
  uint32_t last_sent_entity_count_{UINT32_MAX};
  TimePoint last_sent_load_time_{};
  static constexpr float kInformCellLoadDelta = 0.01f;
  static constexpr Duration kInformCellLoadHeartbeat = std::chrono::seconds(1);

  // Peer CellApp channels. Shared registry (atlas_server) so both
  // BaseApp and CellApp route through the same Birth/Death +
  // self-filter code.
  CellAppPeerRegistry peer_registry_;

  // Trusted BaseApp source addresses — any inbound ClientCellRpcForward
  // whose wire src isn't in this set is dropped. Populated via
  // machined Birth/Death for ProcessType::kBaseApp in Init. An
  // unregistered sender forging ClientCellRpcForward would bypass
  // BaseApp's L1/L2 validation, so this trust gate is a hard
  // constraint.
  std::unordered_set<Address> trusted_baseapps_;

 public:
  // Test-only hook — synthetic tests bypass machined subscription so
  // they need to seed trusted BaseApps directly. Production callers
  // don't touch this; the Subscribe callbacks in Init own the writes.
  void InsertTrustedBaseAppForTest(const Address& addr) { trusted_baseapps_.insert(addr); }

 private:
  std::unordered_map<EntityID, PendingOffload> pending_offloads_;

  // Monotonic epoch for CurrentCell ordering. Incremented each time this
  // CellApp sends a CurrentCell to BaseApp after an Offload arrival, so
  // BaseApp can reject stale updates from a slower old-CellApp path.
  uint32_t next_offload_epoch_{1};

  // Scan pending_offloads_ for entries past the Ack deadline; revert
  // them in place. Called each tick from OnEndOfTick.
  void TickOffloadAckTimeouts();

  static constexpr Duration kOffloadAckTimeout = std::chrono::seconds(5);

  // Safety ceiling on per-tick AvatarUpdate displacement. Reject beyond
  // 50 m/tick (roughly 500 m/s at 10 Hz — well above any realistic
  // player speed).
  static constexpr float kMaxSingleTickMove = 50.f;

  // The provider's concrete type so handlers can reach CellApp-specific
  // state without a dynamic_cast. Base ptr stored in ScriptApp; we keep
  // a typed alias.
  CellAppNativeProvider* native_provider_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_H_
