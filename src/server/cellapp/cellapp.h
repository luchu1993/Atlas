#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_H_

#include <memory>
#include <unordered_map>

#include "cellappmgr/cellappmgr_messages.h"  // cellappmgr::CellID
#include "network/address.h"
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

// ============================================================================
// CellApp — spatial-simulation server process
//
// Inherits from EntityApp (same level as BaseApp) and wires:
//   • Space map — the local set of spatial partitions this cell serves
//   • Dual entity index (cell_id → *, base_id → *) — RPC / AoI route on
//     base_id (phase10_cellapp.md §9.6) while internal management uses
//     the cell-local id
//   • CellAppNativeProvider — C# ↔ C++ interop anchored to this CellApp's
//     entity lookup
//   • Message handlers for the nine CellApp inbound messages (IDs 3000-3099)
//
// Tick flow (phase10_cellapp.md §3.7):
//   OnStartOfTick  — drives EntityApp's C# on_tick
//   … Updatables …
//   C# on_tick → publish_replication_frame via NativeApi
//   OnTickComplete — drives TickControllers(dt) then TickWitnesses()
//   OnEndOfTick    — reserved for future inter-cell channel flush (Phase 11)
// ============================================================================

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

  // ---- Phase 11 inter-CellApp handlers ----
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

  // ---- Phase 11 CellAppMgr → CellApp handlers ----
  void OnAddCellToSpace(const Address& src, Channel* ch, const cellappmgr::AddCellToSpace& msg);
  void OnUpdateGeometry(const Address& src, Channel* ch, const cellappmgr::UpdateGeometry& msg);
  void OnShouldOffload(const Address& src, Channel* ch, const cellappmgr::ShouldOffload& msg);
  void OnRegisterCellAppAck(const Address& src, Channel* ch,
                            const cellappmgr::RegisterCellAppAck& msg);

  // ---- app_id / EntityID bookkeeping (PR-5, §9.6 Q8 scheme A) --------
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

  // Insert a peer Channel* directly. Production uses are Init's machined
  // subscription; tests use it to wire fake channels for Offload flow
  // coverage without running machined.
  void SetPeerChannel(const Address& addr, Channel* ch);

  // ---- Offload orchestration (exposed for tests) --------------------------

  // Build an OffloadEntity message for `entity`. Does NOT send; caller
  // decides transport. `persistent_blob` is left empty here — PR-6
  // wires the C# SerializeEntity callback that fills it in.
  auto BuildOffloadMessage(const CellEntity& entity, cellappmgr::CellID target_cell_id) const
      -> cellapp::OffloadEntity;

  // Ghost-pump + offload-checker pass, called from OnEndOfTick. Exposed
  // so unit tests can step the tick pipeline deterministically.
  void TickGhostPump();
  void TickOffloadChecker();

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
  [[nodiscard]] auto AllocateCellEntityId() -> EntityID;

  std::unordered_map<SpaceID, std::unique_ptr<Space>> spaces_;

  // cell_entity_id → CellEntity*. Non-owning — the owning unique_ptr lives
  // in the peer Space's entities_ map. Holds BOTH Real and Ghost entities
  // (PR-4: §9.6 Q8 — EntityIDs are cluster-wide so no collision risk).
  std::unordered_map<EntityID, CellEntity*> entity_population_;

  // base_entity_id → CellEntity*. Required for RPC routing because
  // clients and BaseApp both identify entities by their base_entity_id,
  // which is stable across CellApp offloads (Phase 11). Only Real
  // entities appear here — client RPCs never dispatch to a Ghost.
  std::unordered_map<EntityID, CellEntity*> base_entity_population_;

  EntityID next_entity_id_{1};

  // app_id assigned by CellAppMgr's RegisterCellAppAck. Forms the high
  // 8 bits of every EntityID we mint (§9.6 Q8 scheme A).
  // 0 ⇒ not yet registered; IDs allocated before registration use
  // high byte 0 and will clash once a CellAppMgr is in the cluster,
  // so callers should avoid creating entities pre-registration in
  // production. Phase 10 unit tests never observe app_id and continue
  // to work with app_id_ == 0.
  uint32_t app_id_{0};
  Channel* cellappmgr_channel_{nullptr};

  // Peer CellApp channels keyed by each peer's internal RUDP address.
  // Populated by the machined Birth subscription in Init; cleared by
  // the matching Death callback.
  std::unordered_map<Address, Channel*> peer_cellapp_channels_;

  // Safety ceiling on per-tick AvatarUpdate displacement. phase10_cellapp.md
  // §3.12 Phase 10 strategy: reject beyond 50 m/tick (roughly 500 m/s at
  // 10 Hz — well above any realistic player speed). Replaced by a per-class
  // maxSpeed in Phase 11.
  static constexpr float kMaxSingleTickMove = 50.f;

  // The provider's concrete type so handlers can reach CellApp-specific
  // state without a dynamic_cast. Base ptr stored in ScriptApp; we keep
  // a typed alias.
  CellAppNativeProvider* native_provider_{nullptr};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_H_
