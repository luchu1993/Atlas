#include "cellapp.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "cell.h"
#include "cell_entity.h"
#include "cellapp_messages.h"
#include "cellapp_native_provider.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "foundation/clock.h"
#include "foundation/log.h"
#include "ghost_maintainer.h"
#include "intercell_messages.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "offload_checker.h"
#include "real_entity_data.h"
#include "server/machined_client.h"
#include "server/server_config.h"
#include "space.h"
#include "witness.h"

namespace atlas {

// ============================================================================
// CellApp::Run — static entry point
// ============================================================================

auto CellApp::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher("cellapp");
  NetworkInterface network(dispatcher);
  CellApp app(dispatcher, network);
  return app.RunApp(argc, argv);
}

CellApp::CellApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : EntityApp(dispatcher, network) {}

CellApp::~CellApp() = default;

// ============================================================================
// Lifecycle
// ============================================================================

auto CellApp::CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> {
  auto provider =
      std::make_unique<CellAppNativeProvider>([this](uint32_t id) { return FindEntity(id); });
  // Keep a typed alias so the CellApp-specific API surface is available
  // to callers without downcasting. ScriptApp owns the unique_ptr.
  native_provider_ = provider.get();
  return provider;
}

auto CellApp::Init(int argc, char* argv[]) -> bool {
  if (!EntityApp::Init(argc, argv)) return false;

  auto& table = Network().InterfaceTable();

  // Message handlers — IDs 3000-3099, see cellapp_messages.h.
  // Each lambda forwards to the matching OnXxx method with the raw
  // source address / channel for any handler-specific routing needs.
  (void)table.RegisterTypedHandler<cellapp::CreateCellEntity>(
      [this](const Address& src, Channel* ch, const cellapp::CreateCellEntity& msg) {
        OnCreateCellEntity(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::DestroyCellEntity>(
      [this](const Address& src, Channel* ch, const cellapp::DestroyCellEntity& msg) {
        OnDestroyCellEntity(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::ClientCellRpcForward>(
      [this](const Address& src, Channel* ch, const cellapp::ClientCellRpcForward& msg) {
        OnClientCellRpcForward(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::InternalCellRpc>(
      [this](const Address& src, Channel* ch, const cellapp::InternalCellRpc& msg) {
        OnInternalCellRpc(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::CreateSpace>(
      [this](const Address& src, Channel* ch, const cellapp::CreateSpace& msg) {
        OnCreateSpace(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::DestroySpace>(
      [this](const Address& src, Channel* ch, const cellapp::DestroySpace& msg) {
        OnDestroySpace(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::AvatarUpdate>(
      [this](const Address& src, Channel* ch, const cellapp::AvatarUpdate& msg) {
        OnAvatarUpdate(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::EnableWitness>(
      [this](const Address& src, Channel* ch, const cellapp::EnableWitness& msg) {
        OnEnableWitness(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::DisableWitness>(
      [this](const Address& src, Channel* ch, const cellapp::DisableWitness& msg) {
        OnDisableWitness(src, ch, msg);
      });

  // ---- Phase 11 inter-CellApp handlers ----------------------------------
  (void)table.RegisterTypedHandler<cellapp::CreateGhost>(
      [this](const Address& src, Channel* ch, const cellapp::CreateGhost& msg) {
        OnCreateGhost(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::DeleteGhost>(
      [this](const Address& src, Channel* ch, const cellapp::DeleteGhost& msg) {
        OnDeleteGhost(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::GhostPositionUpdate>(
      [this](const Address& src, Channel* ch, const cellapp::GhostPositionUpdate& msg) {
        OnGhostPositionUpdate(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::GhostDelta>(
      [this](const Address& src, Channel* ch, const cellapp::GhostDelta& msg) {
        OnGhostDelta(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::GhostSnapshotRefresh>(
      [this](const Address& src, Channel* ch, const cellapp::GhostSnapshotRefresh& msg) {
        OnGhostSnapshotRefresh(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::GhostSetReal>(
      [this](const Address& src, Channel* ch, const cellapp::GhostSetReal& msg) {
        OnGhostSetReal(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::GhostSetNextReal>(
      [this](const Address& src, Channel* ch, const cellapp::GhostSetNextReal& msg) {
        OnGhostSetNextReal(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::OffloadEntity>(
      [this](const Address& src, Channel* ch, const cellapp::OffloadEntity& msg) {
        OnOffloadEntity(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellapp::OffloadEntityAck>(
      [this](const Address& src, Channel* ch, const cellapp::OffloadEntityAck& msg) {
        OnOffloadEntityAck(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::AddCellToSpace>(
      [this](const Address& src, Channel* ch, const cellappmgr::AddCellToSpace& msg) {
        OnAddCellToSpace(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::UpdateGeometry>(
      [this](const Address& src, Channel* ch, const cellappmgr::UpdateGeometry& msg) {
        OnUpdateGeometry(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::ShouldOffload>(
      [this](const Address& src, Channel* ch, const cellappmgr::ShouldOffload& msg) {
        OnShouldOffload(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::RegisterCellAppAck>(
      [this](const Address& src, Channel* ch, const cellappmgr::RegisterCellAppAck& msg) {
        OnRegisterCellAppAck(src, ch, msg);
      });

  // Connect to CellAppMgr on birth + send RegisterCellApp. The manager
  // will respond with RegisterCellAppAck carrying our app_id.
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBirth, ProcessType::kCellAppMgr,
      [this](const machined::BirthNotification& n) {
        if (cellappmgr_channel_ != nullptr) return;
        auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
        if (!ch) {
          ATLAS_LOG_ERROR("CellApp: failed to connect to CellAppMgr at {}:{}", n.internal_addr.Ip(),
                          n.internal_addr.Port());
          return;
        }
        cellappmgr_channel_ = static_cast<Channel*>(*ch);
        cellappmgr::RegisterCellApp reg;
        reg.internal_addr = Network().RudpAddress();
        (void)cellappmgr_channel_->SendMessage(reg);
        ATLAS_LOG_INFO("CellApp: registering with CellAppMgr at {}:{}", n.internal_addr.Ip(),
                       n.internal_addr.Port());
      },
      nullptr);

  // Subscribe to peer CellApps so the Ghost/Offload pipelines can resolve
  // Address → Channel* at will. Filter out ourselves (machined relays
  // birth for the process that just registered).
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kCellApp,
      [this](const machined::BirthNotification& n) {
        if (n.internal_addr == Network().RudpAddress()) return;
        if (peer_cellapp_channels_.contains(n.internal_addr)) return;
        ATLAS_LOG_INFO("CellApp: peer CellApp born at {}:{}", n.internal_addr.Ip(),
                       n.internal_addr.Port());
        auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
        if (ch) peer_cellapp_channels_[n.internal_addr] = static_cast<Channel*>(*ch);
      },
      [this](const machined::DeathNotification& n) {
        ATLAS_LOG_WARNING("CellApp: peer CellApp died at {}:{}", n.internal_addr.Ip(),
                          n.internal_addr.Port());
        peer_cellapp_channels_.erase(n.internal_addr);
      });

  ATLAS_LOG_INFO("CellApp: initialised");
  return true;
}

void CellApp::Fini() {
  // Clear spaces before EntityApp tears down the script engine —
  // CellEntity destructors touch the Witness (which may publish final
  // events) and we want that to happen while script state is still alive.
  spaces_.clear();
  entity_population_.clear();
  base_entity_population_.clear();
  EntityApp::Fini();
}

void CellApp::OnEndOfTick() {
  // Phase 11: drive Ghost maintenance + Offload checks at tick tail so
  // controllers (which may have moved entities) have already committed
  // this frame's positions. GhostMaintainer runs first so a just-
  // crossed entity still has its haunts published before the Offload
  // handoff.
  TickGhostPump();
  TickOffloadChecker();
}

void CellApp::OnTickComplete() {
  // ScriptApp::OnTickComplete drives the C# on_tick (which in turn calls
  // BuildAndConsumeReplicationFrame + publish_replication_frame via
  // NativeApi). Run it first so replication state is current before we
  // tick controllers / witnesses.
  EntityApp::OnTickComplete();

  const auto dt_secs = std::chrono::duration_cast<Seconds>(GetGameClock().FrameDelta()).count();
  TickControllers(static_cast<float>(dt_secs));
  TickWitnesses();
}

void CellApp::RegisterWatchers() {
  EntityApp::RegisterWatchers();
  // Phase 10 §3.11 telemetry surfaces land here. Left empty for the
  // Step 10.8 skeleton so the process boots cleanly; follow-up work
  // adds entities / spaces / bandwidth budget gauges.
}

// ============================================================================
// Tick helpers
// ============================================================================

void CellApp::TickControllers(float dt) {
  for (auto& [_, space] : spaces_) space->Tick(dt);
}

void CellApp::TickWitnesses() {
  // Budget and fan-out policy live in phase10_cellapp.md §3.11. 4 KB/tick
  // per observer is the baseline Phase 10 target; tests exercise the
  // heap + catch-up plumbing regardless of the exact budget.
  constexpr uint32_t kPerObserverBudget = 4096;
  for (auto& [_, space] : spaces_) {
    space->ForEachEntity([](CellEntity& e) {
      if (auto* w = e.GetWitness()) w->Update(kPerObserverBudget);
    });
  }
}

// ============================================================================
// Lookup
// ============================================================================

auto CellApp::FindEntity(EntityID cell_id) -> CellEntity* {
  auto it = entity_population_.find(cell_id);
  return it == entity_population_.end() ? nullptr : it->second;
}

auto CellApp::FindEntityByBaseId(EntityID base_id) -> CellEntity* {
  auto it = base_entity_population_.find(base_id);
  return it == base_entity_population_.end() ? nullptr : it->second;
}

auto CellApp::FindSpace(SpaceID id) -> Space* {
  auto it = spaces_.find(id);
  return it == spaces_.end() ? nullptr : it->second.get();
}

auto CellApp::AllocateCellEntityId() -> EntityID {
  // §9.6 Q8 scheme A — high 8 bits = app_id, low 24 bits = CellApp-local
  // monotonic counter. Until RegisterCellAppAck lands, app_id_ == 0 and
  // we produce Phase-10-compatible IDs that work for single-CellApp
  // tests. In production, Init synchronously registers before any
  // CreateCellEntity traffic arrives.
  const uint32_t local = next_entity_id_++ & 0x00FFFFFFu;
  return (app_id_ << 24) | local;
}

// ============================================================================
// Handlers
// ============================================================================

void CellApp::OnCreateCellEntity(const Address& src, Channel* ch,
                                 const cellapp::CreateCellEntity& msg) {
  // Find or lazily-create the target space — the management path that
  // drives CreateSpace may not have run yet if this is the first entity
  // in a fresh space.
  if (msg.space_id == kInvalidSpaceID) {
    ATLAS_LOG_WARNING("CellApp: CreateCellEntity rejected: space_id == kInvalidSpaceID");
    return;
  }
  auto* space = FindSpace(msg.space_id);
  if (!space) {
    auto inserted = spaces_.emplace(msg.space_id, std::make_unique<Space>(msg.space_id));
    space = inserted.first->second.get();
    ATLAS_LOG_INFO("CellApp: auto-created Space {} for CreateCellEntity", msg.space_id);
  }

  const EntityID cell_id = AllocateCellEntityId();
  auto entity_ptr =
      std::make_unique<CellEntity>(cell_id, msg.type_id, *space, msg.position, msg.direction);
  entity_ptr->SetOnGround(msg.on_ground);
  entity_ptr->SetBase(msg.base_addr, msg.base_entity_id);
  auto* entity = space->AddEntity(std::move(entity_ptr));
  entity_population_[cell_id] = entity;
  base_entity_population_[msg.base_entity_id] = entity;

  // Phase 11: register the new Real with the appropriate local Cell.
  // Use the BSP tree (if present) to pick which local Cell owns this
  // position; fall back to any single local Cell when geometry isn't
  // published yet. Without a local Cell the entity still exists in
  // entity_population_ but no OffloadChecker/GhostMaintainer entry
  // will fire — acceptable in single-CellApp mode.
  if (auto* tree = space->GetBspTree(); tree != nullptr) {
    if (const auto* info = tree->FindCell(msg.position.x, msg.position.z)) {
      if (auto* cell = space->FindLocalCell(info->cell_id)) cell->AddRealEntity(entity);
    }
  } else if (!space->LocalCells().empty()) {
    // Single-cell space: use the only local cell.
    space->LocalCells().begin()->second->AddRealEntity(entity);
  }

  // Respond to the originating BaseApp. The acknowledgement carries our
  // internal address — BaseApp uses it as the cell_addr for this entity
  // going forward.
  if (ch != nullptr) {
    baseapp::CellEntityCreated ack;
    ack.base_entity_id = msg.base_entity_id;
    ack.cell_entity_id = cell_id;
    ack.cell_addr = Network().RudpAddress();
    (void)ch->SendMessage(ack);
  }

  // Hydrate the C# entity object. The restore callback deserializes the
  // blob into the generated entity class and calls OnInit(isRestore=false).
  if (native_provider_ && native_provider_->restore_entity_fn()) {
    ClearNativeApiError();
    native_provider_->restore_entity_fn()(
        cell_id, msg.type_id, /*dbid=*/0,
        reinterpret_cast<const uint8_t*>(msg.script_init_data.data()),
        static_cast<int32_t>(msg.script_init_data.size()));
    if (auto error = ConsumeNativeApiError()) {
      ATLAS_LOG_ERROR("CellApp: RestoreEntity failed cell_id={} type={}: {}", cell_id, msg.type_id,
                      *error);
    }
  }
  (void)src;
}

void CellApp::OnDestroyCellEntity(const Address& /*src*/, Channel* /*ch*/,
                                  const cellapp::DestroyCellEntity& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: DestroyCellEntity for unknown base_id={}", msg.base_entity_id);
    return;
  }
  const EntityID cell_id = entity->Id();

  // Notify C# so the managed entity object can be collected.
  if (native_provider_ && native_provider_->entity_destroyed_fn()) {
    native_provider_->entity_destroyed_fn()(cell_id);
  }

  // Drop from any local Cell membership list before the Space erases
  // the owning unique_ptr.
  for (auto& [_, cell] : entity->GetSpace().LocalCells()) {
    cell->RemoveRealEntity(entity);
  }

  base_entity_population_.erase(msg.base_entity_id);
  entity_population_.erase(cell_id);
  entity->GetSpace().RemoveEntity(cell_id);
}

void CellApp::OnClientCellRpcForward(const Address& /*src*/, Channel* /*ch*/,
                                     const cellapp::ClientCellRpcForward& msg) {
  // Four-layer defence — phase10_cellapp.md §3.8.1. BaseApp already
  // validated direction + exposed + cross-entity rules, but re-checking
  // here is cheap and keeps this handler self-defensive.
  auto* entity = FindEntityByBaseId(msg.target_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: ClientCellRpcForward for unknown target base_id={} rpc_id=0x{:06X}",
                      msg.target_entity_id, msg.rpc_id);
    return;
  }
  // Phase 11: a client-bound RPC must never dispatch on a Ghost. If one
  // arrived, BaseApp's routing is stale — log-and-skip (Q2 soft guard).
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING(
        "CellApp: ClientCellRpcForward for Ghost base_id={} rpc_id=0x{:06X} — stale routing",
        msg.target_entity_id, msg.rpc_id);
    return;
  }

  const auto* rpc = EntityDefRegistry::Instance().FindRpc(msg.rpc_id);
  if (!rpc) {
    ATLAS_LOG_WARNING("CellApp: ClientCellRpcForward unknown rpc_id=0x{:06X}", msg.rpc_id);
    return;
  }
  if (rpc->Direction() != 0x02) {
    ATLAS_LOG_WARNING(
        "CellApp: ClientCellRpcForward rpc_id=0x{:06X} has direction {}, expected 0x02", msg.rpc_id,
        rpc->Direction());
    return;
  }
  if (rpc->exposed == ExposedScope::kNone) {
    ATLAS_LOG_WARNING("CellApp: ClientCellRpcForward rpc_id=0x{:06X} is not exposed", msg.rpc_id);
    return;
  }
  if (rpc->exposed == ExposedScope::kOwnClient && msg.source_entity_id != msg.target_entity_id) {
    ATLAS_LOG_WARNING(
        "CellApp: OwnClient rpc_id=0x{:06X} target={} source={} — cross-entity blocked", msg.rpc_id,
        msg.target_entity_id, msg.source_entity_id);
    return;
  }

  // Dispatch to C# via the native callback. entity->Id() is the
  // cell_entity_id — that's the key C# registered the entity under
  // (phase10_cellapp.md §9.6).
  auto dispatch_fn = native_provider_ ? native_provider_->dispatch_rpc_fn() : nullptr;
  if (!dispatch_fn) {
    ATLAS_LOG_WARNING("CellApp: ClientCellRpcForward: dispatch_rpc callback not registered");
    return;
  }
  dispatch_fn(entity->Id(), msg.rpc_id, reinterpret_cast<const uint8_t*>(msg.payload.data()),
              static_cast<int32_t>(msg.payload.size()));
}

void CellApp::OnInternalCellRpc(const Address& /*src*/, Channel* /*ch*/,
                                const cellapp::InternalCellRpc& msg) {
  // Server-internal Base → Cell call. BaseApp is trusted; skip the
  // exposed / sourceEntityID validation (see phase10_cellapp.md §3.8.2).
  auto* entity = FindEntityByBaseId(msg.target_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: InternalCellRpc for unknown target base_id={} rpc_id=0x{:06X}",
                      msg.target_entity_id, msg.rpc_id);
    return;
  }
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING(
        "CellApp: InternalCellRpc for Ghost base_id={} rpc_id=0x{:06X} — stale routing",
        msg.target_entity_id, msg.rpc_id);
    return;
  }
  // Dispatch directly to C# — internal path, no validation needed.
  auto dispatch_fn = native_provider_ ? native_provider_->dispatch_rpc_fn() : nullptr;
  if (!dispatch_fn) {
    ATLAS_LOG_WARNING("CellApp: InternalCellRpc: dispatch_rpc callback not registered");
    return;
  }
  dispatch_fn(entity->Id(), msg.rpc_id, reinterpret_cast<const uint8_t*>(msg.payload.data()),
              static_cast<int32_t>(msg.payload.size()));
}

void CellApp::OnCreateSpace(const Address& /*src*/, Channel* /*ch*/,
                            const cellapp::CreateSpace& msg) {
  if (msg.space_id == kInvalidSpaceID) {
    ATLAS_LOG_WARNING("CellApp: CreateSpace rejected: space_id == kInvalidSpaceID");
    return;
  }
  if (spaces_.contains(msg.space_id)) {
    ATLAS_LOG_WARNING("CellApp: CreateSpace for existing space_id={}", msg.space_id);
    return;
  }
  spaces_.emplace(msg.space_id, std::make_unique<Space>(msg.space_id));
}

void CellApp::OnDestroySpace(const Address& /*src*/, Channel* /*ch*/,
                             const cellapp::DestroySpace& msg) {
  auto it = spaces_.find(msg.space_id);
  if (it == spaces_.end()) {
    ATLAS_LOG_WARNING("CellApp: DestroySpace for unknown space_id={}", msg.space_id);
    return;
  }
  // Drop all entity-population entries that belong to this space.
  auto& space = *it->second;
  space.ForEachEntity([this](CellEntity& e) {
    entity_population_.erase(e.Id());
    base_entity_population_.erase(e.BaseEntityId());
  });
  spaces_.erase(it);
}

void CellApp::OnAvatarUpdate(const Address& /*src*/, Channel* /*ch*/,
                             const cellapp::AvatarUpdate& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: AvatarUpdate for unknown base_id={}", msg.base_entity_id);
    return;
  }

  // Phase 10 §3.12 safety:
  //   1. NaN / Inf rejection.
  //   2. Per-tick displacement cap — anything bigger is treated as a
  //      teleport attempt from a compromised client.
  if (!std::isfinite(msg.position.x) || !std::isfinite(msg.position.y) ||
      !std::isfinite(msg.position.z)) {
    ATLAS_LOG_WARNING("AvatarUpdate rejected: non-finite position base_id={}", msg.base_entity_id);
    return;
  }
  if (!std::isfinite(msg.direction.x) || !std::isfinite(msg.direction.y) ||
      !std::isfinite(msg.direction.z)) {
    ATLAS_LOG_WARNING("AvatarUpdate rejected: non-finite direction base_id={}", msg.base_entity_id);
    return;
  }
  const math::Vector3 delta{msg.position.x - entity->Position().x,
                            msg.position.y - entity->Position().y,
                            msg.position.z - entity->Position().z};
  const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  if (dist > kMaxSingleTickMove) {
    ATLAS_LOG_WARNING("AvatarUpdate rejected (teleport): base_id={} delta={:.2f}m > {:.2f}m limit",
                      msg.base_entity_id, dist, kMaxSingleTickMove);
    return;
  }

  entity->SetPositionAndDirection(msg.position, msg.direction);
  entity->SetOnGround(msg.on_ground);
}

void CellApp::OnEnableWitness(const Address& /*src*/, Channel* /*ch*/,
                              const cellapp::EnableWitness& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: EnableWitness for unknown base_id={}", msg.base_entity_id);
    return;
  }
  // Wire delivery callbacks to real BaseApp messages. The lambdas
  // capture `this` to access Network() for RUDP channel establishment
  // and base_entity_population_ for observer → BaseApp address lookup.
  entity->EnableWitness(
      msg.aoi_radius,
      // Reliable path — AoI enter/leave + ordered property deltas.
      // Routed via ReplicatedReliableDeltaFromCell (msg 2017) which
      // bypasses DeltaForwarder and reaches the client reliably.
      [this](EntityID observer_base_id, std::span<const std::byte> env) {
        auto* observer = FindEntityByBaseId(observer_base_id);
        if (!observer) return;
        auto ch = Network().ConnectRudpNocwnd(observer->BaseAddr());
        if (!ch) return;
        baseapp::ReplicatedReliableDeltaFromCell msg;
        msg.base_entity_id = observer_base_id;
        msg.delta.assign(env.begin(), env.end());
        (void)(*ch)->SendMessage(msg);
      },
      // Unreliable path — volatile position/orientation (latest-wins).
      // Routed via ReplicatedDeltaFromCell (msg 2015) which goes through
      // DeltaForwarder's per-entity replacement logic.
      [this](EntityID observer_base_id, std::span<const std::byte> env) {
        auto* observer = FindEntityByBaseId(observer_base_id);
        if (!observer) return;
        auto ch = Network().ConnectRudpNocwnd(observer->BaseAddr());
        if (!ch) return;
        baseapp::ReplicatedDeltaFromCell msg;
        msg.base_entity_id = observer_base_id;
        msg.delta.assign(env.begin(), env.end());
        (void)(*ch)->SendMessage(msg);
      });
}

void CellApp::OnDisableWitness(const Address& /*src*/, Channel* /*ch*/,
                               const cellapp::DisableWitness& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) return;
  entity->DisableWitness();
}

// ============================================================================
// Phase 11 — Ghost / Offload handlers
// ============================================================================

auto CellApp::FindPeerChannel(const Address& addr) const -> Channel* {
  auto it = peer_cellapp_channels_.find(addr);
  return it == peer_cellapp_channels_.end() ? nullptr : it->second;
}

void CellApp::SetPeerChannel(const Address& addr, Channel* ch) {
  if (ch == nullptr) {
    peer_cellapp_channels_.erase(addr);
  } else {
    peer_cellapp_channels_[addr] = ch;
  }
}

void CellApp::OnCreateGhost(const Address& /*src*/, Channel* ch, const cellapp::CreateGhost& msg) {
  auto* space = FindSpace(msg.space_id);
  if (!space) {
    auto inserted = spaces_.emplace(msg.space_id, std::make_unique<Space>(msg.space_id));
    space = inserted.first->second.get();
    ATLAS_LOG_INFO("CellApp: auto-created Space {} for incoming Ghost", msg.space_id);
  }
  if (entity_population_.contains(msg.real_entity_id)) {
    ATLAS_LOG_WARNING("CellApp: CreateGhost for already-present entity_id={} — ignoring",
                      msg.real_entity_id);
    return;
  }
  // Resolve a Channel* to the Real CellApp. CreateGhost arrives on the
  // peer's connection (`ch`) so we can just latch that.
  auto* entity_ptr_raw = space->AddEntity(
      std::make_unique<CellEntity>(CellEntity::GhostTag{}, msg.real_entity_id, msg.type_id, *space,
                                   msg.position, msg.direction, ch));
  entity_ptr_raw->SetOnGround(msg.on_ground);
  entity_ptr_raw->SetBase(msg.base_addr, msg.base_entity_id);
  // Seed ghost replication state from the initial snapshot so subsequent
  // GhostDelta frames have a baseline to append to.
  if (!msg.other_snapshot.empty() || msg.event_seq > 0) {
    entity_ptr_raw->GhostApplySnapshot(msg.event_seq,
                                       std::span<const std::byte>(msg.other_snapshot));
  }
  // Seed the volatile seq so the first GhostPositionUpdate is accepted
  // only when it advances beyond this starting point.
  if (msg.volatile_seq > 0) {
    entity_ptr_raw->GhostUpdatePosition(msg.position, msg.direction, msg.on_ground,
                                        msg.volatile_seq);
  }
  entity_population_[msg.real_entity_id] = entity_ptr_raw;
  // Ghosts do NOT register in base_entity_population_; client RPCs must
  // route to the Real (via BaseApp's CurrentCell table, PR-6).
}

void CellApp::OnDeleteGhost(const Address& /*src*/, Channel* /*ch*/,
                            const cellapp::DeleteGhost& msg) {
  auto it = entity_population_.find(msg.ghost_entity_id);
  if (it == entity_population_.end()) {
    ATLAS_LOG_WARNING("CellApp: DeleteGhost for unknown entity_id={}", msg.ghost_entity_id);
    return;
  }
  auto* entity = it->second;
  if (!entity->IsGhost()) {
    ATLAS_LOG_WARNING("CellApp: DeleteGhost for non-Ghost entity_id={} — rejected",
                      msg.ghost_entity_id);
    return;
  }
  entity_population_.erase(it);
  entity->GetSpace().RemoveEntity(entity->Id());
}

void CellApp::OnGhostPositionUpdate(const Address& /*src*/, Channel* /*ch*/,
                                    const cellapp::GhostPositionUpdate& msg) {
  auto it = entity_population_.find(msg.ghost_entity_id);
  if (it == entity_population_.end()) return;  // Ghost not here; drop.
  it->second->GhostUpdatePosition(msg.position, msg.direction, msg.on_ground, msg.volatile_seq);
}

void CellApp::OnGhostDelta(const Address& /*src*/, Channel* /*ch*/,
                           const cellapp::GhostDelta& msg) {
  auto it = entity_population_.find(msg.ghost_entity_id);
  if (it == entity_population_.end()) return;
  it->second->GhostApplyDelta(msg.event_seq, std::span<const std::byte>(msg.other_delta));
}

void CellApp::OnGhostSnapshotRefresh(const Address& /*src*/, Channel* /*ch*/,
                                     const cellapp::GhostSnapshotRefresh& msg) {
  auto it = entity_population_.find(msg.ghost_entity_id);
  if (it == entity_population_.end()) return;
  it->second->GhostApplySnapshot(msg.event_seq, std::span<const std::byte>(msg.other_snapshot));
}

void CellApp::OnGhostSetReal(const Address& /*src*/, Channel* /*ch*/,
                             const cellapp::GhostSetReal& msg) {
  // Our Ghost's authoritative Real just moved. Swap the back-channel to
  // the new Real's peer channel.
  auto it = entity_population_.find(msg.ghost_entity_id);
  if (it == entity_population_.end()) return;
  auto* entity = it->second;
  if (!entity->IsGhost()) return;
  auto* new_ch = FindPeerChannel(msg.new_real_addr);
  if (new_ch == nullptr) {
    ATLAS_LOG_WARNING("CellApp: GhostSetReal for new_real_addr={}:{} — no peer channel",
                      msg.new_real_addr.Ip(), msg.new_real_addr.Port());
    return;
  }
  // ConvertRealToGhost is the only "re-parent" we have; since we're
  // already a Ghost, bypass and overwrite real_channel_ via a targeted
  // helper. Exposing that would widen the Convert API; instead we
  // fall back to Convert-then-Convert for now, which is a no-op because
  // we're already Ghost. Simpler: reset next_real_addr and rely on
  // ghost's stale channel being overwritten by the message pathway. Not
  // ideal — documenting as a known-edge for later refinement.
  entity->SetNextRealAddr({});
  // NOTE: CellEntity currently lacks a SetRealChannel setter. The
  // channel on the Ghost is only changed through ConvertRealToGhost.
  // For PR-4 completeness, we leave the old channel in place; the
  // Ghost's replication still works because cross-process messages
  // arrive from the NEW Real via the interface table (identified by
  // ghost_entity_id, not by channel identity). Deferred refactor:
  // expose a dedicated CellEntity::SetRealChannel and plumb here.
  (void)new_ch;
}

void CellApp::OnGhostSetNextReal(const Address& /*src*/, Channel* /*ch*/,
                                 const cellapp::GhostSetNextReal& msg) {
  auto it = entity_population_.find(msg.ghost_entity_id);
  if (it == entity_population_.end()) return;
  it->second->SetNextRealAddr(msg.next_real_addr);
}

void CellApp::OnOffloadEntity(const Address& src, Channel* ch, const cellapp::OffloadEntity& msg) {
  auto* space = FindSpace(msg.space_id);
  if (!space) {
    // No Space here yet — create it to host the incoming Real.
    auto inserted = spaces_.emplace(msg.space_id, std::make_unique<Space>(msg.space_id));
    space = inserted.first->second.get();
    ATLAS_LOG_INFO("CellApp: auto-created Space {} for incoming Offload", msg.space_id);
  }

  CellEntity* entity = nullptr;
  auto it = entity_population_.find(msg.real_entity_id);
  if (it != entity_population_.end()) {
    // We already had a Ghost here — promote it to Real. The existing
    // replication_state_ is kept by ConvertGhostToReal, and we layer
    // the offload-carried owner snapshot on top.
    entity = it->second;
    if (!entity->IsGhost()) {
      ATLAS_LOG_ERROR(
          "CellApp: OffloadEntity for existing non-Ghost entity_id={} — aborting offload",
          msg.real_entity_id);
      cellapp::OffloadEntityAck ack;
      ack.real_entity_id = msg.real_entity_id;
      ack.success = false;
      if (ch != nullptr) (void)ch->SendMessage(ack);
      return;
    }
    entity->ConvertGhostToReal();
  } else {
    // Fresh Real — no preceding Ghost on this CellApp.
    auto entity_ptr = std::make_unique<CellEntity>(msg.real_entity_id, msg.type_id, *space,
                                                   msg.position, msg.direction);
    entity_ptr->SetOnGround(msg.on_ground);
    entity_ptr->SetBase(msg.base_addr, msg.base_entity_id);
    entity = space->AddEntity(std::move(entity_ptr));
    entity_population_[msg.real_entity_id] = entity;
  }

  // Install base-ID routing so subsequent RPCs land.
  base_entity_population_[msg.base_entity_id] = entity;

  // Seed replication snapshots from the offload payload. Neither call
  // writes through the Ghost path — we're Real here.
  CellEntity::ReplicationFrame frame;
  frame.event_seq = msg.latest_event_seq;
  frame.volatile_seq = msg.latest_volatile_seq;
  frame.position = msg.position;
  frame.direction = msg.direction;
  frame.on_ground = msg.on_ground;
  entity->PublishReplicationFrame(std::move(frame), std::span<const std::byte>(msg.owner_snapshot),
                                  std::span<const std::byte>(msg.other_snapshot));

  // Seed existing haunts so subsequent Ghost messages from this Real
  // don't create duplicate Ghosts on the same peer.
  if (auto* rd = entity->GetRealData()) {
    for (const auto& haunt_addr : msg.existing_haunts) {
      if (haunt_addr == Network().RudpAddress()) continue;  // Don't haunt ourselves.
      if (auto* peer = FindPeerChannel(haunt_addr)) {
        rd->AddHaunt(peer);
      }
    }
  }

  // Register with the target local Cell if one exists at this position.
  if (auto* tree = space->GetBspTree(); tree != nullptr) {
    if (const auto* info = tree->FindCell(msg.position.x, msg.position.z)) {
      if (auto* cell = space->FindLocalCell(info->cell_id)) cell->AddRealEntity(entity);
    }
  } else if (!space->LocalCells().empty()) {
    space->LocalCells().begin()->second->AddRealEntity(entity);
  }

  // Notify BaseApp that the Cell for this entity has moved. PR-6
  // completes the BaseApp-side multi-CellApp routing table; until then
  // this message still needs sending for single-CellApp compatibility.
  baseapp::CurrentCell cc;
  cc.base_entity_id = msg.base_entity_id;
  cc.cell_entity_id = entity->Id();
  cc.cell_addr = Network().RudpAddress();
  auto base_ch = Network().ConnectRudpNocwnd(msg.base_addr);
  if (base_ch) (void)(*base_ch)->SendMessage(cc);

  // Tell existing haunts (except us and the sender) to redirect their
  // back-channel to us. The sender itself will convert its Real to a
  // Ghost — it's already in that state by the time we receive Offload.
  cellapp::GhostSetReal ghost_set_real;
  ghost_set_real.ghost_entity_id = msg.real_entity_id;
  ghost_set_real.new_real_addr = Network().RudpAddress();
  for (const auto& haunt_addr : msg.existing_haunts) {
    if (haunt_addr == Network().RudpAddress()) continue;
    if (haunt_addr == src) continue;
    if (auto* peer = FindPeerChannel(haunt_addr)) {
      (void)peer->SendMessage(ghost_set_real);
    }
  }

  // Ack the offload so the sender can finalise its Real→Ghost transition.
  cellapp::OffloadEntityAck ack;
  ack.real_entity_id = msg.real_entity_id;
  ack.success = true;
  if (ch != nullptr) (void)ch->SendMessage(ack);
}

void CellApp::OnOffloadEntityAck(const Address& /*src*/, Channel* /*ch*/,
                                 const cellapp::OffloadEntityAck& msg) {
  // Sender side: our Real is already a Ghost (TickOffloadChecker
  // converted it before sending OffloadEntity). A success ack is the
  // normal path — nothing more to do. A failure ack is informational
  // for now; a future hardening pass could convert the Ghost back.
  if (!msg.success) {
    ATLAS_LOG_ERROR("CellApp: OffloadEntityAck reported failure for entity_id={}",
                    msg.real_entity_id);
  }
}

// ---- CellAppMgr → CellApp control ----

void CellApp::OnAddCellToSpace(const Address& /*src*/, Channel* /*ch*/,
                               const cellappmgr::AddCellToSpace& msg) {
  auto* space = FindSpace(msg.space_id);
  if (!space) {
    auto inserted = spaces_.emplace(msg.space_id, std::make_unique<Space>(msg.space_id));
    space = inserted.first->second.get();
  }
  if (space->FindLocalCell(msg.cell_id) != nullptr) {
    ATLAS_LOG_WARNING("CellApp: AddCellToSpace for already-present cell_id={}", msg.cell_id);
    return;
  }
  space->AddLocalCell(std::make_unique<Cell>(*space, msg.cell_id, msg.bounds));
  ATLAS_LOG_INFO("CellApp: added Cell {} to Space {}", msg.cell_id, msg.space_id);
}

void CellApp::OnUpdateGeometry(const Address& /*src*/, Channel* /*ch*/,
                               const cellappmgr::UpdateGeometry& msg) {
  auto* space = FindSpace(msg.space_id);
  if (!space) {
    ATLAS_LOG_WARNING("CellApp: UpdateGeometry for unknown space_id={}", msg.space_id);
    return;
  }
  BinaryReader r(std::span<const std::byte>(msg.bsp_blob));
  auto tree = BSPTree::Deserialize(r);
  if (!tree) {
    ATLAS_LOG_ERROR("CellApp: UpdateGeometry BSP deserialize failed: {}", tree.Error().Message());
    return;
  }
  space->SetBspTree(std::move(*tree));
  // Propagate new bounds into local Cells by consulting the new tree.
  for (auto& [cell_id, cell] : space->LocalCells()) {
    if (const auto* info = space->GetBspTree()->FindCellById(cell_id)) {
      cell->SetBounds(info->bounds);
    }
  }
}

void CellApp::OnShouldOffload(const Address& /*src*/, Channel* /*ch*/,
                              const cellappmgr::ShouldOffload& msg) {
  auto* space = FindSpace(msg.space_id);
  if (!space) return;
  if (auto* cell = space->FindLocalCell(msg.cell_id)) {
    cell->SetShouldOffload(msg.enable);
  }
}

void CellApp::OnRegisterCellAppAck(const Address& /*src*/, Channel* /*ch*/,
                                   const cellappmgr::RegisterCellAppAck& msg) {
  if (!msg.success) {
    ATLAS_LOG_ERROR("CellApp: RegisterCellAppAck reported failure from CellAppMgr");
    return;
  }
  if (msg.app_id == 0 || msg.app_id > 0xFFu) {
    ATLAS_LOG_ERROR("CellApp: RegisterCellAppAck invalid app_id={} (expected 1..255)", msg.app_id);
    return;
  }
  app_id_ = msg.app_id;
  ATLAS_LOG_INFO("CellApp: registered with CellAppMgr; app_id={}", app_id_);
}

// ============================================================================
// Tick-time Ghost pump + Offload checker
// ============================================================================

auto CellApp::BuildOffloadMessage(const CellEntity& entity,
                                  cellappmgr::CellID /*target_cell_id*/) const
    -> cellapp::OffloadEntity {
  cellapp::OffloadEntity msg;
  msg.real_entity_id = entity.Id();
  msg.type_id = entity.TypeId();
  msg.space_id = entity.GetSpace().Id();
  msg.position = entity.Position();
  msg.direction = entity.Direction();
  msg.on_ground = entity.OnGround();
  msg.base_addr = entity.BaseAddr();
  msg.base_entity_id = entity.BaseEntityId();
  // PR-6 fills persistent_blob via the SerializeEntity NativeCallback.
  // For PR-4 we ship the replication baseline only; the receiver uses it
  // to serve AoI until the first post-offload C# publish.
  if (const auto* state = entity.GetReplicationState()) {
    msg.owner_snapshot = state->owner_snapshot;
    msg.other_snapshot = state->other_snapshot;
    msg.latest_event_seq = state->latest_event_seq;
    msg.latest_volatile_seq = state->latest_volatile_seq;
  }
  if (const auto* rd = entity.GetRealData()) {
    msg.existing_haunts.reserve(rd->Haunts().size());
    for (const auto& h : rd->Haunts()) {
      if (h.channel != nullptr) msg.existing_haunts.push_back(h.channel->RemoteAddress());
    }
  }
  return msg;
}

void CellApp::TickGhostPump() {
  GhostMaintainer::Config config{};
  const auto resolver = [this](const Address& a) -> Channel* { return FindPeerChannel(a); };
  GhostMaintainer maintainer(config, Network().RudpAddress(), resolver);

  for (auto& [_, space] : spaces_) {
    auto work = maintainer.Run(*space, Clock::now());

    // Dispatch creates: emit CreateGhost on each peer's channel and
    // record the resulting haunt back on the Real's sidecar.
    for (auto& op : work.creates) {
      auto* peer = FindPeerChannel(op.peer_addr);
      if (peer == nullptr) continue;
      auto* rd = op.entity->GetRealData();
      if (rd == nullptr) continue;

      cellapp::CreateGhost msg;
      msg.real_entity_id = op.entity->Id();
      msg.type_id = op.entity->TypeId();
      msg.space_id = op.entity->GetSpace().Id();
      msg.position = op.entity->Position();
      msg.direction = op.entity->Direction();
      msg.on_ground = op.entity->OnGround();
      msg.real_cellapp_addr = Network().RudpAddress();
      msg.base_addr = op.entity->BaseAddr();
      msg.base_entity_id = op.entity->BaseEntityId();
      if (const auto* state = op.entity->GetReplicationState()) {
        msg.event_seq = state->latest_event_seq;
        msg.volatile_seq = state->latest_volatile_seq;
        msg.other_snapshot = state->other_snapshot;
      }
      if (peer->SendMessage(msg)) rd->AddHaunt(peer);
    }

    // Dispatch deletes: DeleteGhost on the peer channel, then drop the
    // haunt from the Real's list.
    for (auto& op : work.deletes) {
      if (op.peer_channel == nullptr) continue;
      cellapp::DeleteGhost msg;
      msg.ghost_entity_id = op.entity->Id();
      (void)op.peer_channel->SendMessage(msg);
      if (auto* rd = op.entity->GetRealData()) rd->RemoveHaunt(op.peer_channel);
    }

    // After structural updates, push position/delta broadcasts for each
    // Real that advanced its seqs since the last pump.
    space->ForEachEntity([&](CellEntity& entity) {
      if (!entity.IsReal()) return;
      auto* rd = entity.GetRealData();
      if (rd == nullptr || rd->HauntCount() == 0) return;
      const auto* state = entity.GetReplicationState();
      if (state == nullptr) return;

      if (state->latest_volatile_seq > rd->LastBroadcastVolatileSeq()) {
        const auto msg = rd->BuildPositionUpdate();
        for (const auto& h : rd->Haunts()) {
          if (h.channel) (void)h.channel->SendMessage(msg);
        }
        rd->MarkBroadcastVolatileSeq(state->latest_volatile_seq);
      }
      if (state->latest_event_seq > rd->LastBroadcastEventSeq()) {
        const auto msg = rd->BuildDelta();
        for (const auto& h : rd->Haunts()) {
          if (h.channel) (void)h.channel->SendMessage(msg);
        }
        rd->MarkBroadcastEventSeq(state->latest_event_seq);
      }
    });
  }
}

void CellApp::TickOffloadChecker() {
  OffloadChecker checker(Network().RudpAddress());
  for (auto& [_, space] : spaces_) {
    auto ops = checker.Compute(*space);
    for (auto& op : ops) {
      auto* peer = FindPeerChannel(op.target_cellapp_addr);
      if (peer == nullptr) {
        ATLAS_LOG_WARNING("CellApp: Offload target {}:{} has no peer channel — deferring",
                          op.target_cellapp_addr.Ip(), op.target_cellapp_addr.Port());
        continue;
      }

      // Step 1-3: build message and warn existing haunts that Real is moving.
      auto msg = BuildOffloadMessage(*op.entity, op.target_cell_id);
      if (auto* rd = op.entity->GetRealData()) {
        cellapp::GhostSetNextReal notify;
        notify.ghost_entity_id = op.entity->Id();
        notify.next_real_addr = op.target_cellapp_addr;
        for (const auto& h : rd->Haunts()) {
          if (h.channel) (void)h.channel->SendMessage(notify);
        }
      }

      // Step 4: send the OffloadEntity to the target.
      if (!peer->SendMessage(msg)) {
        ATLAS_LOG_ERROR("CellApp: OffloadEntity send failed for entity_id={}", op.entity->Id());
        continue;
      }

      // Step 5 (sender): local Real → Ghost immediately, using the peer
      // channel as the new back-channel. Drops witness + controllers per
      // Phase 11 §3.1 / §3.5 Step 9.
      op.entity->ConvertRealToGhost(peer);

      // Remove from all local Cells — we're no longer authoritative here.
      for (auto& [_cell_id, cell] : space->LocalCells()) {
        cell->RemoveRealEntity(op.entity);
      }
      // Ghost should NOT be in base_entity_population_ (client RPCs must
      // route to the new Real).
      base_entity_population_.erase(op.entity->BaseEntityId());
    }
  }
}

}  // namespace atlas
