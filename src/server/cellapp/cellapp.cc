#include "cellapp.h"

#include <chrono>
#include <cmath>
#include <memory>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "cell.h"
#include "cell_entity.h"
#include "cellapp_config.h"
#include "cellapp_messages.h"
#include "cellapp_native_provider.h"
#include "cellappmgr/cellappmgr_messages.h"
#include "controller_codec.h"
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
    : EntityApp(dispatcher, network), peer_registry_(network) {}

CellApp::~CellApp() = default;

// ============================================================================
// Lifecycle
// ============================================================================

auto CellApp::CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> {
  auto provider = std::make_unique<CellAppNativeProvider>(
      [this](uint32_t id) { return FindEntity(id); }, Network());
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
  (void)table.RegisterTypedHandler<cellapp::SetAoIRadius>(
      [this](const Address& src, Channel* ch, const cellapp::SetAoIRadius& msg) {
        OnSetAoIRadius(src, ch, msg);
      });

  // ---- Inter-CellApp handlers ----------------------------------
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

  // Subscribe to peer CellApps via the shared registry. self_addr is
  // our own RUDP address so machined's re-broadcast of our own Birth
  // doesn't add us to the peer list. The death hook sweeps Ghosts that
  // lost their Real + Haunt lists that still reference the dying peer.
  peer_registry_.Subscribe(
      GetMachinedClient(), /*self_addr=*/Network().RudpAddress(),
      [this](const Address& addr, Channel* dying) { OnPeerCellAppDeath(addr, dying); });

  // Track BaseApp peers so OnClientCellRpcForward can reject spoofed
  // senders. Addresses only — we don't send anything back through this
  // channel directly; responses ride the same Channel* the handler
  // received on.
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kBaseApp,
      [this](const machined::BirthNotification& n) {
        trusted_baseapps_.insert(n.internal_addr);
        ATLAS_LOG_INFO("CellApp: trusted BaseApp at {}:{}", n.internal_addr.Ip(),
                       n.internal_addr.Port());
      },
      [this](const machined::DeathNotification& n) {
        if (trusted_baseapps_.erase(n.internal_addr) > 0) {
          ATLAS_LOG_INFO("CellApp: untrusted BaseApp at {}:{}", n.internal_addr.Ip(),
                         n.internal_addr.Port());
        }
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
  // Drive Ghost maintenance + Offload checks at tick tail so
  // controllers (which may have moved entities) have already committed
  // this frame's positions. GhostMaintainer runs first so a just-
  // crossed entity still has its haunts published before the Offload
  // handoff.
  //
  // ServerApp tick order per server_app.h:
  //   OnEndOfTick (this) → OnStartOfTick → Updatables (C# on_tick) →
  //   OnTickComplete (CellApp: TickControllers + TickWitnesses)
  // An entity offloaded here becomes a Ghost BEFORE this frame's C#
  // tick runs. TickWitnesses then skips it (HasWitness() is false after
  // ConvertRealToGhost); the "witness teardown before controller stop"
  // ordering is preserved by the Convert method itself.
  TickGhostPump();
  TickOffloadChecker();
  TickOffloadAckTimeouts();
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
  TickBackupPump();
  TickClientBaselinePump();

  // updateLoad + informOfLoad run every tick. The smoother reads the
  // PREVIOUS tick's work time (set by ServerApp::AdvanceTime after this
  // method returns); first-tick-after-boot reports 0, which CellAppMgr
  // already tolerates.
  UpdatePersistentLoad();
  SendInformCellLoad();
}

void CellApp::RegisterWatchers() {
  EntityApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<float>("cellapp/load", std::function<float()>([this] { return persistent_load_; }));
  wr.Add<uint32_t>("cellapp/real_entity_count",
                   std::function<uint32_t()>([this] { return NumRealEntities(); }));
  wr.Add<std::size_t>("cellapp/total_entity_count",
                      std::function<std::size_t()>([this] { return entity_population_.size(); }));
  wr.Add<std::size_t>("cellapp/space_count",
                      std::function<std::size_t()>([this] { return spaces_.size(); }));
}

// ============================================================================
// Tick helpers
// ============================================================================

void CellApp::TickControllers(float dt) {
  for (auto& [_, space] : spaces_) space->Tick(dt);
}

void CellApp::TickWitnesses() {
  const uint32_t budget = CellAppConfig::WitnessPerObserverBudgetBytes();
  for (auto& [_, space] : spaces_) {
    space->ForEachEntity([budget](CellEntity& e) {
      if (auto* w = e.GetWitness()) w->Update(budget);
    });
  }
}

void CellApp::TickBackupPump() {
  ++backup_tick_counter_;
  if (backup_tick_counter_ % kBackupIntervalTicks != 0) return;

  // SerializeEntity callback is optional — no C# runtime registered
  // (unit tests, early boot, scriptless nodes) means nothing to
  // snapshot. The callback returns CELL_DATA only, so the bytes are
  // safe to ship verbatim without leaking base-scope state.
  if (native_provider_ == nullptr || native_provider_->serialize_entity_fn() == nullptr) {
    return;
  }
  auto fn = native_provider_->serialize_entity_fn();

  for (const auto& [base_id, entity] : base_entity_population_) {
    if (entity == nullptr || !entity->IsReal()) continue;
    const Address& base_addr = entity->BaseAddr();
    if (base_addr.Ip() == 0) continue;  // no base binding yet — skip until next pump

    // Size probe + fetch, same two-phase protocol as BuildOffloadMessage.
    // Keeps buffer allocation on the C++ side.
    int32_t probe_out_len = 0;
    const int32_t probe = fn(entity->Id(), /*out_buf=*/nullptr, /*cap=*/0, &probe_out_len);
    const int32_t needed = probe > 0 ? probe : (probe == 0 ? probe_out_len : -1);
    if (needed <= 0) continue;

    std::vector<std::byte> blob(static_cast<std::size_t>(needed));
    int32_t real_len = 0;
    const int32_t rc = fn(entity->Id(), reinterpret_cast<uint8_t*>(blob.data()), needed, &real_len);
    if (rc != 0 || real_len <= 0) {
      ATLAS_LOG_WARNING(
          "CellApp: SerializeEntity for backup pump failed (base_id={}, rc={}, real_len={})",
          base_id, rc, real_len);
      continue;
    }
    blob.resize(static_cast<std::size_t>(real_len));

    auto base_ch = Network().ConnectRudpNocwnd(base_addr);
    if (!base_ch) continue;  // base transiently unreachable — next pump will retry

    baseapp::BackupCellEntity msg;
    msg.base_entity_id = base_id;
    msg.cell_backup_data = std::move(blob);
    (void)(*base_ch)->SendMessage(msg);
  }
}

void CellApp::TickClientBaselinePump() {
  ++client_baseline_tick_counter_;
  if (client_baseline_tick_counter_ % kClientBaselineIntervalTicks != 0) return;

  // GetOwnerSnapshot is optional — unit tests without a C# runtime, or
  // a runtime that doesn't register the callback, leave the slot null.
  // Short-circuit is safe: no baseline means no safety net for
  // reliable="false" but no corruption either.
  if (native_provider_ == nullptr || native_provider_->get_owner_snapshot_fn() == nullptr) {
    return;
  }
  auto fn = native_provider_->get_owner_snapshot_fn();

  for (const auto& [base_id, entity] : base_entity_population_) {
    if (entity == nullptr || !entity->IsReal()) continue;
    // Only emit baselines for entities with an attached client — a
    // CellEntity with no Witness has nothing to deliver the baseline to.
    // Skipping here keeps the cross-cell broadcast traffic proportional
    // to client count rather than entity count.
    if (!entity->HasWitness()) continue;
    const Address& base_addr = entity->BaseAddr();
    if (base_addr.Ip() == 0) continue;  // base binding not yet populated

    // GetOwnerSnapshot writes via a pinned managed buffer that the C#
    // runtime overwrites on every call — copy the span into a local
    // vector before invoking the callback for the next entity. raw may
    // be null if the entity has no owner-visible properties; treat that
    // as "no baseline this tick" rather than an error.
    uint8_t* raw = nullptr;
    int32_t len = 0;
    fn(entity->Id(), &raw, &len);
    if (len <= 0 || raw == nullptr) continue;

    auto base_ch = Network().ConnectRudpNocwnd(base_addr);
    if (!base_ch) continue;  // base transiently unreachable — retry next pump

    baseapp::ReplicatedBaselineFromCell msg;
    msg.base_entity_id = base_id;
    msg.snapshot.assign(reinterpret_cast<const std::byte*>(raw),
                        reinterpret_cast<const std::byte*>(raw) + len);
    (void)(*base_ch)->SendMessage(msg);
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
  // High 8 bits = app_id, low 24 bits = CellApp-local monotonic counter.
  // Until RegisterCellAppAck lands, app_id_ == 0 and we produce IDs
  // that work for single-CellApp tests. In production, Init
  // synchronously registers before any CreateCellEntity traffic
  // arrives.
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
  // If the sender reported an unresolved bind address (0.0.0.0:port from
  // INADDR_ANY), fall back to the incoming channel's RemoteAddress which
  // is the actual peer we just received from. Symmetric to BaseApp's
  // OnCellEntityCreated fixup on the other leg.
  Address base_addr = msg.base_addr;
  if (base_addr.Ip() == 0 && ch != nullptr) {
    base_addr = ch->RemoteAddress();
  }
  entity_ptr->SetBase(base_addr, msg.base_entity_id);
  auto* entity = space->AddEntity(std::move(entity_ptr));
  entity_population_[cell_id] = entity;
  base_entity_population_[msg.base_entity_id] = entity;
  MarkRealCountDirty();

  // Register the new Real with the appropriate local Cell. Use the BSP
  // tree (if present) to pick which local Cell owns this position;
  // fall back to any single local Cell when geometry isn't published
  // yet. Without a local Cell the entity still exists in
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

  // Creating a cell entity does not auto-enable a witness. Client
  // binding (BaseApp::BindClient) sends cellapp::EnableWitness
  // separately, binding witness attachment to client presence rather
  // than entity creation.
  (void)src;
  (void)ch;
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
  MarkRealCountDirty();
}

void CellApp::OnClientCellRpcForward(const Address& src, Channel* /*ch*/,
                                     const cellapp::ClientCellRpcForward& msg) {
  // Trust boundary (hard constraint): accept only from registered
  // BaseApps. An unregistered sender forging this message would bypass
  // BaseApp's L1/L2 validation (proxy binding + Exposed check) — and
  // since the source_entity_id is stamped BY that sender, would let
  // any network peer impersonate any logged-in client for any exposed
  // cell method.
  if (!trusted_baseapps_.contains(src)) {
    ATLAS_LOG_WARNING(
        "CellApp: ClientCellRpcForward from untrusted src {}:{} "
        "(target={}, rpc=0x{:06X}) — dropping",
        src.Ip(), src.Port(), msg.target_entity_id, msg.rpc_id);
    return;
  }

  // BaseApp already validated direction + exposed + cross-entity rules,
  // but re-checking here is cheap and keeps this handler self-defensive.
  auto* entity = FindEntityByBaseId(msg.target_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: ClientCellRpcForward for unknown target base_id={} rpc_id=0x{:06X}",
                      msg.target_entity_id, msg.rpc_id);
    return;
  }
  // A client-bound RPC must never dispatch on a Ghost. If one arrived,
  // BaseApp's routing is stale — log-and-skip (soft guard).
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
  // cell_entity_id — the key C# registered the entity under.
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
  // exposed / sourceEntityID validation.
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
  MarkRealCountDirty();
}

void CellApp::OnAvatarUpdate(const Address& /*src*/, Channel* /*ch*/,
                             const cellapp::AvatarUpdate& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: AvatarUpdate for unknown base_id={}", msg.base_entity_id);
    return;
  }

  // Safety:
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
  // Compare squared distances to avoid a sqrt on every AvatarUpdate
  // (every client sends these at tick rate). The limit squared stays
  // below float32's 2^24 exact-integer window even for a 1 km cap, so
  // precision is not a concern.
  const float dist_sq = delta.x * delta.x + delta.y * delta.y + delta.z * delta.z;
  constexpr float kMaxSingleTickMoveSq = kMaxSingleTickMove * kMaxSingleTickMove;
  if (dist_sq > kMaxSingleTickMoveSq) {
    ATLAS_LOG_WARNING("AvatarUpdate rejected (teleport): base_id={} delta={:.2f}m > {:.2f}m limit",
                      msg.base_entity_id, std::sqrt(dist_sq), kMaxSingleTickMove);
    return;
  }

  entity->SetPositionAndDirection(msg.position, msg.direction);
  entity->SetOnGround(msg.on_ground);
}

void CellApp::AttachWitness(CellEntity& entity, float aoi_radius, float hysteresis) {
  // Wire delivery callbacks to real BaseApp messages. The lambdas
  // capture `this` to access Network() for RUDP channel establishment
  // and base_entity_population_ for observer → BaseApp address lookup.
  entity.EnableWitness(
      aoi_radius,
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
      },
      hysteresis);
}

void CellApp::OnEnableWitness(const Address& /*src*/, Channel* /*ch*/,
                              const cellapp::EnableWitness& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: EnableWitness for unknown base_id={}", msg.base_entity_id);
    return;
  }
  // Radius + hysteresis come from CellAppConfig. Scripts that want a
  // non-default radius follow up with a SetAoIRadius RPC.
  AttachWitness(*entity, CellAppConfig::DefaultAoIRadius(), CellAppConfig::DefaultAoIHysteresis());
}

void CellApp::OnDisableWitness(const Address& /*src*/, Channel* /*ch*/,
                               const cellapp::DisableWitness& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) return;
  entity->DisableWitness();
}

// Runtime AoI mutation from script. The clamp + floor are applied
// inside Witness::SetAoIRadius; this handler is strictly routing +
// Ghost rejection.
void CellApp::OnSetAoIRadius(const Address& /*src*/, Channel* /*ch*/,
                             const cellapp::SetAoIRadius& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: SetAoIRadius for unknown base_id={}", msg.base_entity_id);
    return;
  }
  // A Ghost has no Witness — only the Real side replicates AoI. Scripts
  // that call SetAoIRadius via a stale mailbox must not mutate Ghost
  // state.
  if (!entity->IsReal()) {
    ATLAS_LOG_WARNING("CellApp: SetAoIRadius on Ghost base_id={} — stale routing",
                      msg.base_entity_id);
    return;
  }
  auto* witness = entity->GetWitness();
  if (!witness) {
    ATLAS_LOG_WARNING("CellApp: SetAoIRadius on base_id={} with no witness attached",
                      msg.base_entity_id);
    return;
  }
  witness->SetAoIRadius(msg.radius, msg.hysteresis);
}

// ============================================================================
// Ghost / Offload handlers
// ============================================================================

auto CellApp::FindPeerChannel(const Address& addr) const -> Channel* {
  return peer_registry_.Find(addr);
}

void CellApp::OnPeerCellAppDeath(const Address& addr, Channel* dying) {
  // Collect orphan Ghost ids up-front — we can't erase from
  // entity_population_ while iterating it. Reals get their Haunt list
  // repaired in place; that's map-key-preserving so safe mid-iter.
  std::vector<EntityID> orphan_ghosts;
  uint32_t haunts_cleared = 0;
  for (auto& [id, entity] : entity_population_) {
    if (entity->IsGhost()) {
      if (entity->GetRealChannel() == dying) orphan_ghosts.push_back(id);
    } else if (entity->IsReal()) {
      if (auto* rd = entity->GetRealData()) {
        if (rd->RemoveHaunt(dying)) ++haunts_cleared;
      }
    }
  }

  for (EntityID id : orphan_ghosts) {
    auto it = entity_population_.find(id);
    if (it == entity_population_.end()) continue;
    auto* entity = it->second;
    entity_population_.erase(it);
    entity->GetSpace().RemoveEntity(id);
  }

  if (!orphan_ghosts.empty() || haunts_cleared > 0) {
    ATLAS_LOG_WARNING(
        "CellApp: peer CellApp {}:{} died — dropped {} orphan Ghost(s), cleared {} Haunt(s)",
        addr.Ip(), addr.Port(), orphan_ghosts.size(), haunts_cleared);
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
  if (ch == nullptr) {
    ATLAS_LOG_WARNING("CellApp: CreateGhost for entity_id={} with null channel — ignoring",
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
  // route to the Real (via BaseApp's CurrentCell table).
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
  if (!std::isfinite(msg.position.x) || !std::isfinite(msg.position.y) ||
      !std::isfinite(msg.position.z) || !std::isfinite(msg.direction.x) ||
      !std::isfinite(msg.direction.y) || !std::isfinite(msg.direction.z)) {
    ATLAS_LOG_WARNING("CellApp: GhostPositionUpdate with NaN/Inf for entity_id={} — dropped",
                      msg.ghost_entity_id);
    return;
  }
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
  // Our Ghost's authoritative Real just moved. Rebind the back-channel
  // to the new Real's peer channel so any future forwarded traffic
  // (e.g. reply RPCs) targets the right CellApp.
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
  entity->RebindRealChannel(new_ch);
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
  MarkRealCountDirty();

  // Install base-ID routing so subsequent RPCs land.
  base_entity_population_[msg.base_entity_id] = entity;

  // Restore the C# entity instance from the sender's persistent_blob
  // via RestoreEntity. Empty blob is valid (unit tests / C#-less
  // setups): the Real then has no script state and any client-visible
  // AoI is driven entirely from the replication baseline below, until
  // the first successful C# publish.
  if (!msg.persistent_blob.empty() && native_provider_ != nullptr &&
      native_provider_->restore_entity_fn() != nullptr) {
    ClearNativeApiError();
    native_provider_->restore_entity_fn()(
        entity->Id(), msg.type_id, /*dbid=*/0,
        reinterpret_cast<const uint8_t*>(msg.persistent_blob.data()),
        static_cast<int32_t>(msg.persistent_blob.size()));
    if (auto error = ConsumeNativeApiError()) {
      ATLAS_LOG_ERROR("CellApp: Offload RestoreEntity failed cell_id={} type={}: {}", entity->Id(),
                      msg.type_id, *error);
    }
  }

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
        rd->AddHaunt(peer, haunt_addr);
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

  // Re-attach the Witness with the preserved radius + hysteresis. Runs
  // AFTER PublishReplicationFrame / Cell membership so AoITrigger's
  // initial sweep sees the right peer set + seq baselines. Without
  // this, the client would stop receiving AoI envelopes the moment a
  // Real crosses a cell boundary (BaseApp re-routes cell_addr via
  // CurrentCell but has no channel to auto-fire EnableWitness).
  if (msg.has_witness) {
    AttachWitness(*entity, msg.aoi_radius, msg.aoi_hysteresis);
  }

  // Rebuild controllers from the sender's snapshot. Must run AFTER the
  // entity is registered with entity_population_ + the local Cell so
  // the ProximityController's peer resolver can look up peers by
  // entity_id.
  if (!msg.controller_data.empty()) {
    BinaryReader r(std::span<const std::byte>(msg.controller_data));
    const bool ok = DeserializeControllersForMigration(
        *entity, r, [this](uint32_t peer_id) -> CellEntity* { return FindEntity(peer_id); });
    if (!ok) {
      ATLAS_LOG_ERROR(
          "CellApp: Offload controller restore failed for entity_id={} — arriving "
          "controllers partially / empty",
          msg.real_entity_id);
    }
  }

  // Notify BaseApp that the Cell for this entity has moved. The epoch
  // lets BaseApp reject stale CurrentCell messages that arrive out of
  // order (e.g. from an old CellApp path during rapid re-offload).
  baseapp::CurrentCell cc;
  cc.base_entity_id = msg.base_entity_id;
  cc.cell_entity_id = entity->Id();
  cc.cell_addr = Network().RudpAddress();
  cc.epoch = next_offload_epoch_++;
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
  // On success, drop the pending entry; on failure, auto-revert the
  // Ghost back to Real using the snapshot.
  auto pending_it = pending_offloads_.find(msg.real_entity_id);
  if (pending_it == pending_offloads_.end()) {
    ATLAS_LOG_WARNING(
        "CellApp: OffloadEntityAck for entity_id={} not in pending set — "
        "duplicate ack, unknown entity, or TickOffloadChecker never tracked it",
        msg.real_entity_id);
    return;
  }
  const Address target = pending_it->second.target_addr;
  const auto age = Clock::now() - pending_it->second.sent_at;

  if (msg.success) {
    pending_offloads_.erase(pending_it);
    ATLAS_LOG_INFO("CellApp: Offload of entity_id={} to {}:{} succeeded (rtt={} ms)",
                   msg.real_entity_id, target.Ip(), target.Port(),
                   std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
    return;
  }
  ATLAS_LOG_ERROR(
      "CellApp: Offload of entity_id={} to {}:{} REJECTED after {} ms — reverting Ghost to Real "
      "locally",
      msg.real_entity_id, target.Ip(), target.Port(),
      std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
  RevertPendingOffload(msg.real_entity_id, "receiver rejected");
}

void CellApp::TickOffloadAckTimeouts() {
  if (pending_offloads_.empty()) return;
  const auto now = Clock::now();
  // Collect first so we can mutate the map while reverting.
  std::vector<EntityID> timed_out;
  for (const auto& [eid, po] : pending_offloads_) {
    if (now - po.sent_at >= kOffloadAckTimeout) timed_out.push_back(eid);
  }
  for (auto eid : timed_out) {
    ATLAS_LOG_ERROR(
        "CellApp: Offload of entity_id={} TIMED OUT after {} ms — reverting Ghost to Real locally",
        eid, std::chrono::duration_cast<std::chrono::milliseconds>(kOffloadAckTimeout).count());
    RevertPendingOffload(eid, "ack timeout");
  }
}

void CellApp::RevertPendingOffload(EntityID entity_id, const char* reason) {
  auto pending_it = pending_offloads_.find(entity_id);
  if (pending_it == pending_offloads_.end()) return;
  PendingOffload po = std::move(pending_it->second);
  pending_offloads_.erase(pending_it);

  auto* entity = FindEntity(entity_id);
  if (entity == nullptr) {
    ATLAS_LOG_ERROR("CellApp: RevertPendingOffload entity_id={} not found (reason={}) — drop",
                    entity_id, reason);
    return;
  }
  if (!entity->IsGhost()) {
    // Someone already finalised (e.g. a second Ack arrived success=true
    // on a retried path). Nothing to do.
    ATLAS_LOG_WARNING(
        "CellApp: RevertPendingOffload entity_id={} is not a Ghost — revert skipped (reason={})",
        entity_id, reason);
    return;
  }

  // Ghost → Real. replication_state_ carries over (both Convert methods
  // preserve it), so AoI continues serving from the baseline the ghost
  // was already replicating.
  entity->ConvertGhostToReal();
  MarkRealCountDirty();

  // Re-install in base_entity_population_ so client RPCs that arrive
  // before BaseApp's CurrentCell re-sync still dispatch correctly. The
  // Real currently lives here — even though the BaseApp never learned
  // we moved (CurrentCell is sent by the receiver on success, not the
  // sender on send), so BaseApp's cell_addr for this entity is already
  // us.
  base_entity_population_[entity->BaseEntityId()] = entity;

  // Restore local-Cell membership. The snapshot captured which Cell we
  // lived in pre-Offload; if that Cell is still present we rejoin.
  if (po.cell_id != 0) {
    if (auto* space = FindSpace(po.space_id); space != nullptr) {
      if (auto* cell = space->FindLocalCell(po.cell_id)) cell->AddRealEntity(entity);
    }
  }

  // Restore haunts: resolve each saved peer address through the
  // registry and re-add. Peers that died during the Offload window
  // fall out — they'd come back on the next TickGhostPump pass anyway.
  if (auto* rd = entity->GetRealData()) {
    for (const auto& ha : po.haunt_addrs) {
      if (auto* peer = FindPeerChannel(ha)) rd->AddHaunt(peer, ha);
    }
  }

  // Restore controllers from the serialised blob.
  if (!po.controller_blob.empty()) {
    BinaryReader r{std::span<const std::byte>(po.controller_blob)};
    const bool ok = DeserializeControllersForMigration(
        *entity, r, [this](uint32_t peer_id) -> CellEntity* { return FindEntity(peer_id); });
    if (!ok) {
      ATLAS_LOG_ERROR(
          "CellApp: Offload revert controller restore failed for entity_id={} — controllers lost",
          entity_id);
    }
  }

  // Reattach the witness with the pre-Offload radius/hyst. Either path
  // must produce an identical witness-bearing Real, so revert delegates
  // to the same AttachWitness helper.
  if (po.had_witness) {
    AttachWitness(*entity, po.aoi_radius, po.aoi_hysteresis);
  }

  ATLAS_LOG_INFO(
      "CellApp: Offload revert complete for entity_id={} (reason={}); Real re-installed on local "
      "CellApp",
      entity_id, reason);
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

auto CellApp::NumRealEntities() const -> uint32_t {
  if (!real_count_dirty_) return real_count_cached_;
  uint32_t n = 0;
  for (const auto& [_, ent] : entity_population_) {
    if (ent != nullptr && ent->IsReal()) ++n;
  }
  real_count_cached_ = n;
  real_count_dirty_ = false;
  return n;
}

void CellApp::UpdatePersistentLoad() {
  // Map work_time / expected_tick_period → [0, …] load fraction, then
  // feed the EWMA:
  //   persistent_load = (1-bias) * persistent_load + bias * frac
  const auto work = LastTickWorkDuration();
  const auto expected = ExpectedTickPeriod();
  if (expected.count() <= 0) return;  // defensive: ill-configured hertz
  const double frac =
      std::chrono::duration<double>(work).count() / std::chrono::duration<double>(expected).count();

  const float bias = CellAppConfig::LoadSmoothingBias();
  persistent_load_ = (1.f - bias) * persistent_load_ + bias * static_cast<float>(frac);
}

void CellApp::SendInformCellLoad() {
  if (cellappmgr_channel_ == nullptr || app_id_ == 0) return;
  const auto count = NumRealEntities();
  const auto now = Clock::now();
  const bool first_send = last_sent_load_time_.time_since_epoch().count() == 0;
  const bool load_changed = std::abs(persistent_load_ - last_sent_load_) >= kInformCellLoadDelta;
  const bool count_changed = count != last_sent_entity_count_;
  const bool heartbeat_due =
      !first_send && (now - last_sent_load_time_) >= kInformCellLoadHeartbeat;
  if (!first_send && !load_changed && !count_changed && !heartbeat_due) return;

  cellappmgr::InformCellLoad msg;
  msg.app_id = app_id_;
  msg.load = persistent_load_;
  msg.entity_count = count;
  (void)cellappmgr_channel_->SendMessage(msg);

  last_sent_load_ = persistent_load_;
  last_sent_entity_count_ = count;
  last_sent_load_time_ = now;
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

auto CellApp::BuildOffloadMessage(const CellEntity& entity) const -> cellapp::OffloadEntity {
  cellapp::OffloadEntity msg;
  msg.real_entity_id = entity.Id();
  msg.type_id = entity.TypeId();
  msg.space_id = entity.GetSpace().Id();
  msg.position = entity.Position();
  msg.direction = entity.Direction();
  msg.on_ground = entity.OnGround();
  msg.base_addr = entity.BaseAddr();
  msg.base_entity_id = entity.BaseEntityId();
  // Capture the C# entity state via SerializeEntity if the callback is
  // registered. The destination CellApp's OnOffloadEntity will pass
  // this blob through RestoreEntity. If no runtime is registered (unit
  // tests, early boot) we ship an empty blob and rely on the
  // replication baseline — the Ghost→Real promotion still works, just
  // without script state restoration.
  if (native_provider_ != nullptr && native_provider_->serialize_entity_fn() != nullptr) {
    auto fn = native_provider_->serialize_entity_fn();
    // Size probe + one retry: ask C# for the required size with a
    // zero-cap call, then allocate and fetch. The two-phase protocol
    // keeps buffer allocation on the C++ side (easier to log / cap)
    // and avoids an ambient thread-local on the C# side.
    int32_t needed = 0;
    int32_t probe_out_len = 0;
    const int32_t probe = fn(entity.Id(), /*out_buf=*/nullptr, /*cap=*/0, &probe_out_len);
    if (probe > 0) {
      needed = probe;
    } else if (probe == 0) {
      needed = probe_out_len;
    }
    if (needed > 0) {
      msg.persistent_blob.resize(static_cast<std::size_t>(needed));
      int32_t real_len = 0;
      const int32_t rc = fn(entity.Id(), reinterpret_cast<uint8_t*>(msg.persistent_blob.data()),
                            needed, &real_len);
      if (rc == 0 && real_len > 0) {
        msg.persistent_blob.resize(static_cast<std::size_t>(real_len));
      } else {
        ATLAS_LOG_WARNING(
            "CellApp: SerializeEntity failed (rc={}, real_len={}) — shipping empty blob", rc,
            real_len);
        msg.persistent_blob.clear();
      }
    }
  }
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
  // Serialise live controller state so the Offload receiver can resume
  // MoveToPoint/Timer/Proximity without losing mid-motion progress or
  // proximity membership. Serialised at send-time — before
  // ConvertRealToGhost runs StopAll — so the controllers are still
  // the authoritative live set.
  {
    BinaryWriter cw;
    SerializeControllersForMigration(entity, cw);
    auto buf = cw.Detach();
    msg.controller_data.assign(buf.begin(), buf.end());
  }
  // Preserve Witness state across the Offload. Read BEFORE
  // ConvertRealToGhost (the caller sequences this correctly).
  if (const auto* witness = entity.GetWitness()) {
    msg.has_witness = true;
    msg.aoi_radius = witness->AoIRadius();
    msg.aoi_hysteresis = witness->Hysteresis();
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
      if (peer->SendMessage(msg)) rd->AddHaunt(peer, op.peer_addr);
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
    const auto now = Clock::now();
    const auto min_interval = std::chrono::milliseconds(CellAppConfig::GhostUpdateIntervalMs());
    space->ForEachEntity([&](CellEntity& entity) {
      if (!entity.IsReal()) return;
      auto* rd = entity.GetRealData();
      if (rd == nullptr || rd->HauntCount() == 0) return;
      const auto* state = entity.GetReplicationState();
      if (state == nullptr) return;

      // Throttle per-entity fan-out to the configured interval. A single
      // Real moved every tick would otherwise fan out at tick rate × H
      // haunts; coalescing into one broadcast per interval bounds wire
      // cost at the cost of a little staleness on the Ghost side.
      if (min_interval.count() > 0 && rd->LastBroadcastTime().time_since_epoch().count() != 0 &&
          now - rd->LastBroadcastTime() < min_interval) {
        return;
      }

      bool broadcast_fired = false;
      if (state->latest_volatile_seq > rd->LastBroadcastVolatileSeq()) {
        const auto msg = rd->BuildPositionUpdate();
        for (const auto& h : rd->Haunts()) {
          if (h.channel) (void)h.channel->SendMessage(msg);
        }
        rd->MarkBroadcastVolatileSeq(state->latest_volatile_seq);
        broadcast_fired = true;
      }
      if (state->latest_event_seq > rd->LastBroadcastEventSeq()) {
        // Gap > 1 means multiple frames published since our last
        // broadcast — BuildDelta would only ship the latest frame's
        // other_delta, losing intermediate state. Fall back to a
        // GhostSnapshotRefresh that re-bases the Ghost on the current
        // other_snapshot + latest_event_seq.
        if (RealEntityData::ShouldUseSnapshotRefresh(state->latest_event_seq,
                                                     rd->LastBroadcastEventSeq())) {
          const auto msg = rd->BuildSnapshotRefresh();
          for (const auto& h : rd->Haunts()) {
            if (h.channel) (void)h.channel->SendMessage(msg);
          }
          broadcast_fired = true;
        } else {
          const auto msg = rd->BuildDelta();
          // The DeltaSyncEmitter ships a 1-4 byte flags prefix even
          // when only owner-visible properties were dirty. That
          // produces an all-zero payload with no actual audience
          // content. Skip the SendMessage — the seq still advances, so
          // the Ghost catches up on the next non-empty delta without a
          // spurious snapshot refresh.
          if (!RealEntityData::IsEmptyOtherDelta(msg.other_delta)) {
            for (const auto& h : rd->Haunts()) {
              if (h.channel) (void)h.channel->SendMessage(msg);
            }
            broadcast_fired = true;
          }
        }
        rd->MarkBroadcastEventSeq(state->latest_event_seq);
      }
      if (broadcast_fired) rd->MarkBroadcastTime(now);
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
      auto msg = BuildOffloadMessage(*op.entity);
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

      // Record the outstanding Offload with a full revert snapshot so
      // OnOffloadEntityAck / TickOffloadAckTimeouts can restore a live
      // Real when the receiver rejects or never replies. Snapshot is
      // captured BEFORE ConvertRealToGhost drops the haunt list and
      // StopAlls controllers.
      PendingOffload po;
      po.target_addr = op.target_cellapp_addr;
      po.sent_at = Clock::now();
      po.space_id = op.entity->GetSpace().Id();
      for (const auto& [cid, cell] : space->LocalCells()) {
        if (cell->HasRealEntity(op.entity)) {
          po.cell_id = cid;
          break;
        }
      }
      if (const auto* rd = op.entity->GetRealData()) {
        po.haunt_addrs.reserve(rd->Haunts().size());
        for (const auto& h : rd->Haunts()) {
          if (h.channel != nullptr) po.haunt_addrs.push_back(h.channel->RemoteAddress());
        }
      }
      {
        BinaryWriter cw;
        SerializeControllersForMigration(*op.entity, cw);
        auto buf = cw.Detach();
        po.controller_blob.assign(buf.begin(), buf.end());
      }
      // Capture witness state BEFORE ConvertRealToGhost strips it. On
      // a revert the entity must come back with the same radius/hyst
      // the script had configured — losing it would silently break
      // SetAoIRadius contracts across a failed-Offload boundary.
      if (const auto* witness = op.entity->GetWitness()) {
        po.had_witness = true;
        po.aoi_radius = witness->AoIRadius();
        po.aoi_hysteresis = witness->Hysteresis();
      }
      pending_offloads_[op.entity->Id()] = std::move(po);

      // Step 5 (sender): local Real → Ghost immediately, using the peer
      // channel as the new back-channel. Drops witness + controllers.
      op.entity->ConvertRealToGhost(peer);
      MarkRealCountDirty();

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
