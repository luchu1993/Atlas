#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_H_

#include <memory>
#include <unordered_map>

#include "network/address.h"
#include "server/entity_app.h"
#include "server/entity_types.h"

namespace atlas {

class Space;
class CellEntity;
class CellAppNativeProvider;

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
}  // namespace cellapp

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
  // in the peer Space's entities_ map.
  std::unordered_map<EntityID, CellEntity*> entity_population_;

  // base_entity_id → CellEntity*. Required for RPC routing because
  // clients and BaseApp both identify entities by their base_entity_id,
  // which is stable across CellApp offloads (Phase 11).
  std::unordered_map<EntityID, CellEntity*> base_entity_population_;

  EntityID next_entity_id_{1};

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
