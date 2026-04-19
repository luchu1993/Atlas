#include "cellapp.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "cell_entity.h"
#include "cellapp_messages.h"
#include "cellapp_native_provider.h"
#include "foundation/clock.h"
#include "foundation/log.h"
#include "math/vector3.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
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
  // Reserved for Phase 11 inter-cell channel flush; today a no-op.
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
  // Phase 10 single-CellApp stage: simple monotonic counter. Phase 11
  // introduces a cluster-wide allocator tied to the CellAppMgr.
  return next_entity_id_++;
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

}  // namespace atlas
