#include "cellapp.h"

#include <algorithm>
#include <cassert>
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
#include "foundation/profiler.h"
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

auto CellApp::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher("cellapp");
  NetworkInterface network(dispatcher);
  CellApp app(dispatcher, network);
  return app.RunApp(argc, argv);
}

CellApp::CellApp(EventDispatcher& dispatcher, NetworkInterface& network)
    : EntityApp(dispatcher, network), peer_registry_(network) {}

CellApp::~CellApp() = default;

auto CellApp::CreateNativeProvider() -> std::unique_ptr<INativeApiProvider> {
  auto provider = std::make_unique<CellAppNativeProvider>(
      [this](uint32_t id) { return FindEntity(id); }, Network());
  // Typed alias so callers reach CellApp-specific API without a
  // downcast; ScriptApp owns the unique_ptr.
  native_provider_ = provider.get();
  return provider;
}

auto CellApp::Init(int argc, char* argv[]) -> bool {
  if (!EntityApp::Init(argc, argv)) return false;

  auto& table = Network().InterfaceTable();

  // Message handlers — IDs 3000-3099, see cellapp_messages.h.
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

  // Connect to CellAppMgr on birth and send RegisterCellApp; the
  // manager replies with RegisterCellAppAck carrying our app_id.
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
        if (auto r = cellappmgr_channel_->SendMessage(reg); !r) {
          // No ack ⇒ this CellApp is orphaned (mgr won't route load
          // queries / offloads to us) until the next reconnect attempt.
          ATLAS_LOG_WARNING("CellApp: RegisterCellApp send failed to mgr {}: {}",
                            n.internal_addr.ToString(), r.Error().Message());
        }
        ATLAS_LOG_INFO("CellApp: registering with CellAppMgr at {}:{}", n.internal_addr.Ip(),
                       n.internal_addr.Port());
      },
      nullptr);

  // self_addr filters our own re-broadcast Birth from the peer list.
  // The death hook sweeps Ghosts whose Real lived on the dying peer.
  peer_registry_.Subscribe(
      GetMachinedClient(), /*self_addr=*/Network().RudpAddress(),
      [this](const Address& addr, Channel* dying) { OnPeerCellAppDeath(addr, dying); });

  // Track BaseApp peers so OnClientCellRpcForward can reject spoofed
  // senders. Addresses only — responses ride the same Channel* the
  // handler received on.
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
  ATLAS_PROFILE_ZONE_N("CellApp::OnEndOfTick");
  // Run at tick tail so controllers have committed this frame's
  // positions. GhostMaintainer first so a just-crossed entity still
  // has its haunts published before the Offload handoff.
  TickGhostPump();
  TickOffloadChecker();
  TickOffloadAckTimeouts();
}

void CellApp::OnTickComplete() {
  ATLAS_PROFILE_ZONE_N("CellApp::OnTickComplete");
  // Run base first so the C# on_tick / publish_replication_frame has
  // committed before we tick controllers and witnesses.
  EntityApp::OnTickComplete();

  const auto dt_secs = std::chrono::duration_cast<Seconds>(GetGameClock().FrameDelta()).count();
  TickControllers(static_cast<float>(dt_secs));
  TickWitnesses();
  TickBackupPump();
  TickClientBaselinePump();

  // The smoother reads the PREVIOUS tick's work time (set by
  // ServerApp::AdvanceTime after we return); first-tick-after-boot
  // reports 0, which CellAppMgr already tolerates.
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

void CellApp::TickControllers(float dt) {
  ATLAS_PROFILE_ZONE_N("CellApp::TickControllers");
  for (auto& [_, space] : spaces_) space->Tick(dt);
}

void CellApp::TickWitnesses() {
  ATLAS_PROFILE_ZONE_N("CellApp::TickWitnesses");
  // Demand-based fair share: collect each observer's estimated outbound
  // demand, then scale against the cellapp cap so dense observers
  // don't get starved by an equal-share split.
  const uint32_t cap = CellAppConfig::WitnessTotalOutboundCapBytes();
  const uint32_t min_budget = CellAppConfig::WitnessMinPerObserverBudgetBytes();
  const uint32_t max_budget = CellAppConfig::WitnessMaxPerObserverBudgetBytes();
  const uint32_t per_peer_bytes = CellAppConfig::WitnessPerPeerBytes();

  for (auto& [_, space] : spaces_) {
    witness_demand_scratch_.clear();
    uint64_t total_want = 0;  // 64-bit so dense PvP can't overflow uint32
    space->ForEachEntity([&](CellEntity& e) {
      if (auto* w = e.GetWitness()) {
        const uint32_t want = w->EstimateOutboundDemandBytes(per_peer_bytes);
        witness_demand_scratch_.push_back({w, want});
        total_want += want;
      }
    });

    // When demand fits the cap every observer gets what it asked for;
    // when it doesn't, everyone scales down proportionally until
    // min_budget kicks in as a fairness floor.
    const float scale =
        (total_want > cap) ? static_cast<float>(cap) / static_cast<float>(total_want) : 1.0f;
    for (auto& d : witness_demand_scratch_) {
      const uint32_t budget = std::clamp(static_cast<uint32_t>(static_cast<float>(d.want) * scale),
                                         min_budget, max_budget);
      d.w->Update(budget);
    }
  }
  // Witness sends use SendMessage(kBatched); the framework's tick-end
  // FlushTickDirtyChannels is the single drain point.
}

void CellApp::TickBackupPump() {
  ATLAS_PROFILE_ZONE_N("CellApp::TickBackupPump");
  ++backup_tick_counter_;
  if (backup_tick_counter_ % kBackupIntervalTicks != 0) return;

  // No-op when SerializeEntity isn't registered (tests / early boot /
  // scriptless nodes). The callback returns CELL_DATA only, so the
  // bytes ship verbatim without leaking base-scope state.
  if (native_provider_ == nullptr || native_provider_->serialize_entity_fn() == nullptr) {
    return;
  }
  auto fn = native_provider_->serialize_entity_fn();

  for (const auto& [base_id, entity] : entity_population_) {
    if (entity == nullptr || !entity->IsReal()) continue;
    const Address& base_addr = entity->BaseAddr();
    if (base_addr.Ip() == 0) continue;  // no base binding yet — skip until next pump

    // Two-phase size probe + fetch, same protocol as BuildOffloadMessage.
    int32_t probe_out_len = 0;
    const int32_t probe = fn(entity->Id(), /*out_buf=*/nullptr, /*cap=*/0, &probe_out_len);
    const int32_t needed = probe > 0 ? probe : (probe == 0 ? probe_out_len : -1);
    if (needed <= 0) continue;

    // RUDP carries at most ~370 KB per message; a larger blob would
    // allocate tens of MB only to be dropped with kMessageTooLarge.
    constexpr int32_t kMaxBackupBlobBytes = 256 * 1024;
    if (needed > kMaxBackupBlobBytes) {
      ATLAS_LOG_WARNING(
          "CellApp: SerializeEntity probe returned {} bytes for base_id={} "
          "(limit={}); skipping backup pump for this entity",
          needed, base_id, kMaxBackupBlobBytes);
      continue;
    }

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
    const auto blob_size = msg.cell_backup_data.size();
    auto send_result = (*base_ch)->SendMessage(msg);
    if (!send_result) {
      ATLAS_LOG_WARNING(
          "CellApp: BackupCellEntity send failed (base_id={}, blob_size={}, addr={}): {}", base_id,
          blob_size, base_addr.ToString(), send_result.Error().Message());
    }
  }
}

void CellApp::TickClientBaselinePump() {
  ATLAS_PROFILE_ZONE_N("CellApp::TickClientBaselinePump");
  ++client_baseline_tick_counter_;
  if (client_baseline_tick_counter_ % kClientBaselineIntervalTicks != 0) return;

  // No-op when GetOwnerSnapshot isn't registered (tests / unconfigured
  // runtimes); no baseline means no safety net but no corruption.
  if (native_provider_ == nullptr || native_provider_->get_owner_snapshot_fn() == nullptr) {
    return;
  }
  auto fn = native_provider_->get_owner_snapshot_fn();

  for (const auto& [base_id, entity] : entity_population_) {
    if (entity == nullptr || !entity->IsReal()) continue;
    // Only client-bound (has Witness) entities receive baselines — keeps
    // broadcast traffic proportional to client count, not entity count.
    if (!entity->HasWitness()) continue;
    const Address& base_addr = entity->BaseAddr();
    if (base_addr.Ip() == 0) continue;  // base binding not yet populated

    // GetOwnerSnapshot writes via a pinned managed buffer overwritten on
    // every call; raw==null means no owner-visible properties this tick.
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

auto CellApp::FindEntity(EntityID cell_id) -> CellEntity* {
  auto it = entity_population_.find(cell_id);
  return it == entity_population_.end() ? nullptr : it->second;
}

auto CellApp::FindEntityByBaseId(EntityID base_id) -> CellEntity* {
  // Post-unification: cell_entity_id == base_entity_id, so a single
  // entity_population_ lookup serves both ids. IsReal() preserves the
  // legacy semantics where this entry point only resolves Real entities
  // (client RPCs must never dispatch to a Ghost).
  auto* entity = FindEntity(base_id);
  return (entity != nullptr && entity->IsReal()) ? entity : nullptr;
}

auto CellApp::FindSpace(SpaceID id) -> Space* {
  auto it = spaces_.find(id);
  return it == spaces_.end() ? nullptr : it->second.get();
}

void CellApp::OnCreateCellEntity(const Address& src, Channel* ch,
                                 const cellapp::CreateCellEntity& msg) {
  // Lazily create — the management CreateSpace path may not have run
  // yet if this is the first entity in a fresh space.
  if (msg.space_id == kInvalidSpaceID) {
    ATLAS_LOG_WARNING("CellApp: CreateCellEntity rejected: space_id == kInvalidSpaceID");
    return;
  }
  if (msg.base_entity_id == kInvalidEntityID) {
    ATLAS_LOG_WARNING("CellApp: CreateCellEntity rejected: base_entity_id == kInvalidEntityID");
    return;
  }
  auto* space = FindSpace(msg.space_id);
  if (!space) {
    auto inserted = spaces_.emplace(msg.space_id, std::make_unique<Space>(msg.space_id));
    space = inserted.first->second.get();
    ATLAS_LOG_INFO("CellApp: auto-created Space {} for CreateCellEntity", msg.space_id);
  }

  // cell_id == base_entity_id: DBApp's IDClient is the single cluster
  // authority; CellApp no longer mints its own ids.
  const EntityID cell_id = msg.base_entity_id;
  auto entity_ptr =
      std::make_unique<CellEntity>(cell_id, msg.type_id, *space, msg.position, msg.direction);
  entity_ptr->SetOnGround(msg.on_ground);
  // INADDR_ANY fixup: fall back to the channel's RemoteAddress when the
  // sender reported 0.0.0.0:port. Symmetric to BaseApp's other leg.
  Address base_addr = msg.base_addr;
  if (base_addr.Ip() == 0 && ch != nullptr) {
    base_addr = ch->RemoteAddress();
  }
  entity_ptr->SetBase(base_addr, msg.base_entity_id);
  auto* entity = space->AddEntity(std::move(entity_ptr));
  entity_population_[cell_id] = entity;
  base_entity_population_[msg.base_entity_id] = entity;

  // BSP tree (if present) picks the owning local Cell; otherwise fall
  // back to the only local Cell. No local Cell ⇒ no OffloadChecker /
  // GhostMaintainer entry, acceptable in single-CellApp mode.
  if (auto* tree = space->GetBspTree(); tree != nullptr) {
    if (const auto* info = tree->FindCell(msg.position.x, msg.position.z)) {
      if (auto* cell = space->FindLocalCell(info->cell_id)) cell->AddRealEntity(entity);
    }
  } else if (!space->LocalCells().empty()) {
    space->LocalCells().begin()->second->AddRealEntity(entity);
  }

  // Ack carries our address — BaseApp uses it as cell_addr going forward.
  if (ch != nullptr) {
    baseapp::CellEntityCreated ack;
    ack.base_entity_id = msg.base_entity_id;
    ack.cell_entity_id = cell_id;
    ack.cell_addr = Network().RudpAddress();
    if (auto r = ch->SendMessage(ack); !r) {
      // Dropped ack ⇒ BaseApp times out and resends CreateCellEntity;
      // logging here distinguishes that from a genuinely slow create.
      ATLAS_LOG_WARNING("CellApp: CellEntityCreated ack send failed, base_entity_id={} to {}: {}",
                        msg.base_entity_id, src.ToString(), r.Error().Message());
    }
  }

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

  // No auto-witness here — BaseApp::BindClient sends EnableWitness
  // separately so the witness lifecycle tracks client presence rather
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

  if (native_provider_ && native_provider_->entity_destroyed_fn()) {
    native_provider_->entity_destroyed_fn()(cell_id);
  }

  // Drop local Cell membership before Space erases the owning unique_ptr.
  for (auto& [_, cell] : entity->GetSpace().LocalCells()) {
    cell->RemoveRealEntity(entity);
  }

  base_entity_population_.erase(msg.base_entity_id);
  entity_population_.erase(cell_id);
  entity->GetSpace().RemoveEntity(cell_id);
}

void CellApp::OnClientCellRpcForward(const Address& src, Channel* /*ch*/,
                                     const cellapp::ClientCellRpcForward& msg) {
  // Hard trust boundary: forging from outside the BaseApp set bypasses
  // BaseApp's L1/L2 validation and source_entity_id stamping — letting
  // any peer impersonate any client for any exposed cell method.
  if (!trusted_baseapps_.contains(src)) {
    ATLAS_LOG_WARNING(
        "CellApp: ClientCellRpcForward from untrusted src {}:{} "
        "(target={}, rpc=0x{:06X}) — dropping",
        src.Ip(), src.Port(), msg.target_entity_id, msg.rpc_id);
    return;
  }

  // Re-validate cheaply even though BaseApp already checked direction +
  // exposed + cross-entity rules, so this handler is self-defensive.
  auto* entity = FindEntityByBaseId(msg.target_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: ClientCellRpcForward for unknown target base_id={} rpc_id=0x{:06X}",
                      msg.target_entity_id, msg.rpc_id);
    return;
  }
  // Client RPCs must never dispatch on a Ghost — stale BaseApp routing.
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

  // entity->Id() is the cell_entity_id — the key C# registered under.
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
  // Server-internal Base → Cell call; BaseApp is trusted so skip the
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
  // Drop entity-population entries for everything in this space.
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

  // Reject NaN / Inf; reject displacement > kMaxSingleTickMove as a
  // teleport attempt from a compromised client.
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
  // Compare squared distances to skip the per-tick sqrt; limit² stays
  // inside float32's 2^24 exact-integer window even for a 1 km cap.
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
  entity.EnableWitness(
      aoi_radius,
      // Reliable path — enters/leaves + ordered property deltas via
      // ReplicatedReliableDeltaFromCell (msg 2017). SendMessage's
      // kBatched urgency lets FlushTickDirtyChannels coalesce N×M
      // observer/peer fan-out into one packet per (channel, reliability)
      // per tick.
      [this](EntityID observer_base_id, std::span<const std::byte> env) {
        auto* observer = FindEntityByBaseId(observer_base_id);
        if (!observer) return;
        auto ch = Network().ConnectRudpNocwnd(observer->BaseAddr());
        if (!ch) return;
        baseapp::ReplicatedReliableDeltaFromCellSpan msg{observer_base_id, env};
        (void)(*ch)->SendMessage(msg);
      },
      // Unreliable path — volatile pos/dir (latest-wins) via
      // ReplicatedDeltaFromCell (msg 2015), which routes through
      // DeltaForwarder's per-entity replacement logic.
      [this](EntityID observer_base_id, std::span<const std::byte> env) {
        auto* observer = FindEntityByBaseId(observer_base_id);
        if (!observer) return;
        auto ch = Network().ConnectRudpNocwnd(observer->BaseAddr());
        if (!ch) return;
        baseapp::ReplicatedDeltaFromCellSpan msg{observer_base_id, env};
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
  // Defaults from config; scripts wanting a custom radius follow up
  // with a SetAoIRadius RPC.
  AttachWitness(*entity, CellAppConfig::DefaultAoIRadius(), CellAppConfig::DefaultAoIHysteresis());
}

void CellApp::OnDisableWitness(const Address& /*src*/, Channel* /*ch*/,
                               const cellapp::DisableWitness& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) return;
  entity->DisableWitness();
}

void CellApp::OnSetAoIRadius(const Address& /*src*/, Channel* /*ch*/,
                             const cellapp::SetAoIRadius& msg) {
  auto* entity = FindEntityByBaseId(msg.base_entity_id);
  if (!entity) {
    ATLAS_LOG_WARNING("CellApp: SetAoIRadius for unknown base_id={}", msg.base_entity_id);
    return;
  }
  // Only the Real side replicates AoI — reject stale-mailbox calls
  // landing on a Ghost.
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

auto CellApp::FindPeerChannel(const Address& addr) const -> Channel* {
  return peer_registry_.Find(addr);
}

void CellApp::OnPeerCellAppDeath(const Address& addr, Channel* dying) {
  // Collect orphan Ghost ids first — can't erase while iterating.
  // Reals' Haunt-list repairs are map-key-preserving so safe mid-iter.
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
  // CreateGhost arrives on the peer's connection (ch), so latch it as
  // the Real-side back-channel.
  auto* entity_ptr_raw = space->AddEntity(
      std::make_unique<CellEntity>(CellEntity::GhostTag{}, msg.real_entity_id, msg.type_id, *space,
                                   msg.position, msg.direction, ch));
  entity_ptr_raw->SetOnGround(msg.on_ground);
  entity_ptr_raw->SetBase(msg.base_addr, msg.base_entity_id);
  // Seed snapshot baseline so subsequent GhostDelta frames have
  // something to append to.
  if (!msg.other_snapshot.empty() || msg.event_seq > 0) {
    entity_ptr_raw->GhostApplySnapshot(msg.event_seq,
                                       std::span<const std::byte>(msg.other_snapshot));
  }
  // Seed volatile_seq so the first GhostPositionUpdate is only accepted
  // once it strictly advances.
  if (msg.volatile_seq > 0) {
    entity_ptr_raw->GhostUpdatePosition(msg.position, msg.direction, msg.on_ground,
                                        msg.volatile_seq);
  }
  entity_population_[msg.real_entity_id] = entity_ptr_raw;
  // Ghosts stay out of base_entity_population_ — client RPCs must route
  // to the Real via BaseApp's CurrentCell table.
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
  // Rebind the Ghost's back-channel so future forwarded traffic
  // (e.g. reply RPCs) targets the new Real's CellApp.
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
    auto inserted = spaces_.emplace(msg.space_id, std::make_unique<Space>(msg.space_id));
    space = inserted.first->second.get();
    ATLAS_LOG_INFO("CellApp: auto-created Space {} for incoming Offload", msg.space_id);
  }

  CellEntity* entity = nullptr;
  auto it = entity_population_.find(msg.real_entity_id);
  if (it != entity_population_.end()) {
    // Promote a pre-existing Ghost. ConvertGhostToReal preserves
    // replication_state_; the offload owner_snapshot layers on top.
    entity = it->second;
    if (!entity->IsGhost()) {
      ATLAS_LOG_ERROR(
          "CellApp: OffloadEntity for existing non-Ghost entity_id={} — aborting offload",
          msg.real_entity_id);
      cellapp::OffloadEntityAck ack;
      ack.real_entity_id = msg.real_entity_id;
      ack.success = false;
      if (ch != nullptr) {
        if (auto r = ch->SendMessage(ack); !r) {
          // Dropped failure-ack races a possibly-dropped success-ack
          // and may cause the sender to double-revert.
          ATLAS_LOG_ERROR(
              "CellApp: failed to send OffloadEntityAck(success=false) entity_id={} "
              "to {}: {} — sender may double-revert",
              msg.real_entity_id, src.ToString(), r.Error().Message());
        }
      }
      return;
    }
    entity->ConvertGhostToReal();
  } else {
    auto entity_ptr = std::make_unique<CellEntity>(msg.real_entity_id, msg.type_id, *space,
                                                   msg.position, msg.direction);
    entity_ptr->SetOnGround(msg.on_ground);
    entity_ptr->SetBase(msg.base_addr, msg.base_entity_id);
    entity = space->AddEntity(std::move(entity_ptr));
    entity_population_[msg.real_entity_id] = entity;
  }

  // Offload preserves cell_entity_id end-to-end (sender packs its
  // local id into msg.real_entity_id; receiver promotes the matching
  // ghost or constructs a fresh Real with the same id). base_entity_id
  // is the cluster-stable identity from BaseApp's IDClient.
  assert(entity->Id() == msg.real_entity_id);
  assert(entity->BaseEntityId() == msg.base_entity_id);

  base_entity_population_[msg.base_entity_id] = entity;

  // Empty blob is valid (tests / C#-less setups): the Real has no
  // script state and AoI runs off the replication baseline below until
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

  // Seed replication snapshots — neither call uses the Ghost path
  // (we're Real here).
  CellEntity::ReplicationFrame frame;
  frame.event_seq = msg.latest_event_seq;
  frame.volatile_seq = msg.latest_volatile_seq;
  frame.position = msg.position;
  frame.direction = msg.direction;
  frame.on_ground = msg.on_ground;
  entity->PublishReplicationFrame(std::move(frame), std::span<const std::byte>(msg.owner_snapshot),
                                  std::span<const std::byte>(msg.other_snapshot));

  // Seed existing haunts so subsequent Ghost messages from this Real
  // don't create duplicates on the same peer.
  if (auto* rd = entity->GetRealData()) {
    for (const auto& haunt_addr : msg.existing_haunts) {
      if (haunt_addr == Network().RudpAddress()) continue;  // don't haunt ourselves
      if (auto* peer = FindPeerChannel(haunt_addr)) {
        rd->AddHaunt(peer, haunt_addr);
      }
    }
  }

  if (auto* tree = space->GetBspTree(); tree != nullptr) {
    if (const auto* info = tree->FindCell(msg.position.x, msg.position.z)) {
      if (auto* cell = space->FindLocalCell(info->cell_id)) cell->AddRealEntity(entity);
    }
  } else if (!space->LocalCells().empty()) {
    space->LocalCells().begin()->second->AddRealEntity(entity);
  }

  // Re-attach AFTER PublishReplicationFrame / Cell membership so
  // AoITrigger's initial sweep sees the right peer set + seq baselines.
  // Skipping this would silently drop AoI envelopes across a cell
  // boundary — BaseApp re-routes cell_addr but won't re-fire
  // EnableWitness on its own.
  if (msg.has_witness) {
    AttachWitness(*entity, msg.aoi_radius, msg.aoi_hysteresis);
  }

  // Restore AFTER entity_population_ + local Cell are populated so
  // ProximityController's peer resolver can look up peers by entity_id.
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

  // CurrentCell.epoch lets BaseApp reject stale updates from an old
  // CellApp path during rapid re-offload.
  baseapp::CurrentCell cc;
  cc.base_entity_id = msg.base_entity_id;
  cc.cell_entity_id = entity->Id();
  cc.cell_addr = Network().RudpAddress();
  cc.epoch = next_offload_epoch_++;
  auto base_ch = Network().ConnectRudpNocwnd(msg.base_addr);
  if (base_ch) {
    if (auto r = (*base_ch)->SendMessage(cc); !r) {
      // BaseApp keeps the OLD cell_addr until the next offload, routing
      // ClientCellRpc to a Ghost or dead host. Epoch alone can't
      // reconcile this if CurrentCell never arrives.
      ATLAS_LOG_ERROR(
          "CellApp: failed to send CurrentCell entity_id={} epoch={} to base {}: {} "
          "— BaseApp split-brain risk",
          msg.real_entity_id, cc.epoch, msg.base_addr.ToString(), r.Error().Message());
    }
  }

  // Redirect every existing haunt's back-channel to us. The sender is
  // already in Ghost state by the time Offload arrives here.
  cellapp::GhostSetReal ghost_set_real;
  ghost_set_real.ghost_entity_id = msg.real_entity_id;
  ghost_set_real.new_real_addr = Network().RudpAddress();
  for (const auto& haunt_addr : msg.existing_haunts) {
    if (haunt_addr == Network().RudpAddress()) continue;
    if (haunt_addr == src) continue;
    if (auto* peer = FindPeerChannel(haunt_addr)) {
      if (auto r = peer->SendMessage(ghost_set_real); !r) {
        // Haunt's back-channel stays pointed at the old Real host
        // until OffloadChecker re-discovers the topology; deltas
        // vanish in the meantime.
        ATLAS_LOG_ERROR(
            "CellApp: failed to send GhostSetReal entity_id={} to haunt {}: {} "
            "— haunt back-channel stale",
            msg.real_entity_id, haunt_addr.ToString(), r.Error().Message());
      }
    }
  }

  cellapp::OffloadEntityAck ack;
  ack.real_entity_id = msg.real_entity_id;
  ack.success = true;
  if (ch != nullptr) {
    if (auto r = ch->SendMessage(ack); !r) {
      // Sender will time out and revert Ghost→Real, but we've already
      // converted Ghost→Real locally — two Reals for one entity until
      // the cluster heals.
      ATLAS_LOG_ERROR(
          "CellApp: failed to send OffloadEntityAck(success=true) entity_id={} to {}: {} "
          "— two-Reals split-brain until sender's revert timeout fires",
          msg.real_entity_id, src.ToString(), r.Error().Message());
    }
  }
}

void CellApp::OnOffloadEntityAck(const Address& /*src*/, Channel* /*ch*/,
                                 const cellapp::OffloadEntityAck& msg) {
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
  ATLAS_PROFILE_ZONE_N("CellApp::TickOffloadAckTimeouts");
  const auto now = Clock::now();
  // Collect first so the revert below can mutate the map.
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
    // Someone already finalised (e.g. a retried-path success Ack
    // arrived). Nothing to do.
    ATLAS_LOG_WARNING(
        "CellApp: RevertPendingOffload entity_id={} is not a Ghost — revert skipped (reason={})",
        entity_id, reason);
    return;
  }

  // Both Convert methods preserve replication_state_, so AoI continues
  // serving from the baseline the Ghost was already replicating.
  entity->ConvertGhostToReal();

  // BaseApp's cell_addr is still us (CurrentCell is sent by the
  // receiver on success, not the sender on send), so client RPCs land
  // here — re-install routing before they arrive.
  base_entity_population_[entity->BaseEntityId()] = entity;

  // Restore local-Cell membership if the captured Cell still exists.
  if (po.cell_id != 0) {
    if (auto* space = FindSpace(po.space_id); space != nullptr) {
      if (auto* cell = space->FindLocalCell(po.cell_id)) cell->AddRealEntity(entity);
    }
  }

  // Peers that died during the Offload window fall out — the next
  // TickGhostPump pass would re-add them anyway.
  if (auto* rd = entity->GetRealData()) {
    for (const auto& ha : po.haunt_addrs) {
      if (auto* peer = FindPeerChannel(ha)) rd->AddHaunt(peer, ha);
    }
  }

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

  // Both arrival and revert must produce an identical witness-bearing
  // Real, so revert goes through the same AttachWitness helper.
  if (po.had_witness) {
    AttachWitness(*entity, po.aoi_radius, po.aoi_hysteresis);
  }

  ATLAS_LOG_INFO(
      "CellApp: Offload revert complete for entity_id={} (reason={}); Real re-installed on local "
      "CellApp",
      entity_id, reason);
}

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
  // Propagate new bounds into local Cells from the new tree.
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
  uint32_t n = 0;
  for (const auto& [_, ent] : entity_population_) {
    if (ent != nullptr && ent->IsReal()) ++n;
  }
  return n;
}

void CellApp::UpdatePersistentLoad() {
  // EWMA of work_time / expected_tick_period; bias from config.
  const auto work = LastTickWorkDuration();
  const auto expected = ExpectedTickPeriod();
  if (expected.count() <= 0) return;  // ill-configured hertz
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
  if (auto r = cellappmgr_channel_->SendMessage(msg); !r) {
    // Consistently dropped reports skew the mgr's balancer toward
    // this (apparently lightly loaded) host.
    ATLAS_LOG_WARNING("CellApp: InformCellLoad send failed: {}", r.Error().Message());
  }

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
  // Empty blob is valid (tests / early boot); receiver still promotes
  // Ghost→Real but without script state restoration.
  if (native_provider_ != nullptr && native_provider_->serialize_entity_fn() != nullptr) {
    auto fn = native_provider_->serialize_entity_fn();
    // Two-phase probe-then-fetch keeps buffer allocation on the C++
    // side and avoids an ambient thread-local on the C# side.
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
  // Capture live controller state BEFORE ConvertRealToGhost runs
  // StopAll, so the receiver resumes mid-motion progress + proximity
  // membership intact.
  {
    BinaryWriter cw;
    SerializeControllersForMigration(entity, cw);
    auto buf = cw.Detach();
    msg.controller_data.assign(buf.begin(), buf.end());
  }
  // Read BEFORE ConvertRealToGhost strips the witness (caller sequences
  // this correctly).
  if (const auto* witness = entity.GetWitness()) {
    msg.has_witness = true;
    msg.aoi_radius = witness->AoIRadius();
    msg.aoi_hysteresis = witness->Hysteresis();
  }
  return msg;
}

void CellApp::TickGhostPump() {
  ATLAS_PROFILE_ZONE_N("CellApp::TickGhostPump");
  GhostMaintainer::Config config{};
  const auto resolver = [this](const Address& a) -> Channel* { return FindPeerChannel(a); };
  GhostMaintainer maintainer(config, Network().RudpAddress(), resolver);

  for (auto& [_, space] : spaces_) {
    auto work = maintainer.Run(*space, Clock::now());

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

    for (auto& op : work.deletes) {
      if (op.peer_channel == nullptr) continue;
      cellapp::DeleteGhost msg;
      msg.ghost_entity_id = op.entity->Id();
      (void)op.peer_channel->SendMessage(msg);
      if (auto* rd = op.entity->GetRealData()) rd->RemoveHaunt(op.peer_channel);
    }

    // Push position / delta broadcasts for each Real that advanced its
    // seqs since the last pump.
    const auto now = Clock::now();
    const auto min_interval = std::chrono::milliseconds(CellAppConfig::GhostUpdateIntervalMs());
    space->ForEachEntity([&](CellEntity& entity) {
      if (!entity.IsReal()) return;
      auto* rd = entity.GetRealData();
      if (rd == nullptr || rd->HauntCount() == 0) return;
      const auto* state = entity.GetReplicationState();
      if (state == nullptr) return;

      // Coalesce per-entity fan-out into one broadcast per interval —
      // bounds wire cost at the cost of slight Ghost-side staleness.
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
        // Gap > 1 ⇒ multiple frames published; BuildDelta would only
        // ship the latest, losing intermediate state. Snapshot-refresh
        // re-bases the Ghost on other_snapshot + latest_event_seq.
        if (RealEntityData::ShouldUseSnapshotRefresh(state->latest_event_seq,
                                                     rd->LastBroadcastEventSeq())) {
          const auto msg = rd->BuildSnapshotRefresh();
          for (const auto& h : rd->Haunts()) {
            if (h.channel) (void)h.channel->SendMessage(msg);
          }
          broadcast_fired = true;
        } else {
          const auto msg = rd->BuildDelta();
          // Skip empty / flag-only deltas (owner-only frames produce a
          // 1-4 B flag prefix with no audience content). Seq still
          // advances, so the next non-empty frame doesn't gap-warn.
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
  ATLAS_PROFILE_ZONE_N("CellApp::TickOffloadChecker");
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

      // Build the message and warn existing haunts Real is moving.
      auto msg = BuildOffloadMessage(*op.entity);
      if (auto* rd = op.entity->GetRealData()) {
        cellapp::GhostSetNextReal notify;
        notify.ghost_entity_id = op.entity->Id();
        notify.next_real_addr = op.target_cellapp_addr;
        for (const auto& h : rd->Haunts()) {
          if (h.channel) (void)h.channel->SendMessage(notify);
        }
      }

      if (!peer->SendMessage(msg)) {
        ATLAS_LOG_ERROR("CellApp: OffloadEntity send failed for entity_id={}", op.entity->Id());
        continue;
      }

      // Capture the revert snapshot BEFORE ConvertRealToGhost drops
      // the haunt list and StopAlls controllers, so a rejected /
      // timed-out Offload can restore a live Real.
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
      // Read BEFORE ConvertRealToGhost strips the witness — losing
      // radius/hyst would silently break SetAoIRadius contracts across
      // a failed-Offload boundary.
      if (const auto* witness = op.entity->GetWitness()) {
        po.had_witness = true;
        po.aoi_radius = witness->AoIRadius();
        po.aoi_hysteresis = witness->Hysteresis();
      }
      pending_offloads_[op.entity->Id()] = std::move(po);

      // Local Real → Ghost; drops witness + controllers and uses the
      // peer channel as the new back-channel.
      op.entity->ConvertRealToGhost(peer);

      for (auto& [_cell_id, cell] : space->LocalCells()) {
        cell->RemoveRealEntity(op.entity);
      }
      // Ghosts stay out of base_entity_population_ — client RPCs must
      // route to the new Real.
      base_entity_population_.erase(op.entity->BaseEntityId());
    }
  }
}

}  // namespace atlas
