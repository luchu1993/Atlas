#include "cellappmgr.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

#include "foundation/log.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "serialization/binary_stream.h"
#include "server/machined_client.h"
#include "server/watcher.h"

namespace atlas {

namespace {

// Mirror of BaseAppMgr's helper — rewrites a 0-IP advertised address to
// the packet's actual source so peers behind NAT / on loopback still end
// up reachable.
auto ResolveAdvertisedAddr(const Address& advertised, const Address& src) -> Address {
  if (advertised.Ip() != 0) return advertised;
  return Address(src.Ip(), advertised.Port());
}

}  // namespace

// ============================================================================
// Run / ctor
// ============================================================================

auto CellAppMgr::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher("cellappmgr");
  NetworkInterface network(dispatcher);
  CellAppMgr app(dispatcher, network);
  return app.RunApp(argc, argv);
}

CellAppMgr::CellAppMgr(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network) {}

// ============================================================================
// Lifecycle
// ============================================================================

auto CellAppMgr::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  auto& table = Network().InterfaceTable();

  (void)table.RegisterTypedHandler<cellappmgr::RegisterCellApp>(
      [this](const Address& src, Channel* ch, const cellappmgr::RegisterCellApp& msg) {
        OnRegisterCellApp(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::InformCellLoad>(
      [this](const Address& src, Channel* ch, const cellappmgr::InformCellLoad& msg) {
        OnInformCellLoad(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::CreateSpaceRequest>(
      [this](const Address& src, Channel* ch, const cellappmgr::CreateSpaceRequest& msg) {
        OnCreateSpaceRequest(src, ch, msg);
      });

  // Subscribe to CellApp death notifications so we can drop peers from
  // the routing table. Resurrection / reassignment of their Cells is
  // out of PR-5 scope — see the doc's "初期不实现的功能" table.
  GetMachinedClient().Subscribe(
      machined::ListenerType::kDeath, ProcessType::kCellApp, nullptr,
      [this](const machined::DeathNotification& n) { OnCellAppDeath(n.internal_addr); });

  ATLAS_LOG_INFO("CellAppMgr: initialised");
  return true;
}

void CellAppMgr::Fini() {
  ManagerApp::Fini();
}

void CellAppMgr::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<std::size_t>("cellappmgr/cellapp_count",
                      std::function<std::size_t()>([this] { return cellapps_.size(); }));
  wr.Add<std::size_t>("cellappmgr/space_count",
                      std::function<std::size_t()>([this] { return spaces_.size(); }));
  wr.Add<uint32_t>("cellappmgr/next_app_id",
                   std::function<uint32_t()>([this] { return next_cellapp_app_id_; }));
}

void CellAppMgr::OnTickComplete() {
  ManagerApp::OnTickComplete();
  const auto tick = GameTime();
  if (tick - last_balance_tick_ >= kBalanceTickInterval) {
    last_balance_tick_ = tick;
    TickLoadBalance();
  }
}

// ============================================================================
// Handlers
// ============================================================================

void CellAppMgr::OnRegisterCellApp(const Address& src, Channel* ch,
                                   const cellappmgr::RegisterCellApp& msg) {
  const Address kInternalAddr = ResolveAdvertisedAddr(msg.internal_addr, src);

  if (cellapps_.contains(kInternalAddr)) {
    ATLAS_LOG_WARNING("CellAppMgr: duplicate CellApp registration for {}:{}", kInternalAddr.Ip(),
                      kInternalAddr.Port());
    cellappmgr::RegisterCellAppAck ack;
    ack.success = false;
    if (ch != nullptr) (void)ch->SendMessage(ack);
    return;
  }

  // §9.6 Q8 scheme A — app_id lives in the entity_id high byte, so we
  // cap allocations at 255. If we exhaust that pool the cluster has
  // grown past what the current EntityID layout supports and the fix
  // is a wider scheme, not a work-around here.
  if (next_cellapp_app_id_ > kMaxCellAppAppId) {
    ATLAS_LOG_ERROR(
        "CellAppMgr: CellApp app_id pool exhausted (> {}) — rejecting register from {}:{}",
        kMaxCellAppAppId, kInternalAddr.Ip(), kInternalAddr.Port());
    cellappmgr::RegisterCellAppAck ack;
    ack.success = false;
    if (ch != nullptr) (void)ch->SendMessage(ack);
    return;
  }

  const uint32_t app_id = next_cellapp_app_id_++;
  CellAppInfo info;
  info.internal_addr = kInternalAddr;
  info.app_id = app_id;
  info.channel = ch;
  info.registered_at = Clock::now();
  cellapps_.emplace(kInternalAddr, std::move(info));

  cellappmgr::RegisterCellAppAck ack;
  ack.success = true;
  ack.app_id = app_id;
  ack.game_time = GameTime();
  if (ch != nullptr) (void)ch->SendMessage(ack);

  ATLAS_LOG_INFO("CellAppMgr: CellApp registered app_id={} internal={}:{}", app_id,
                 kInternalAddr.Ip(), kInternalAddr.Port());
}

void CellAppMgr::OnInformCellLoad(const Address& /*src*/, Channel* /*ch*/,
                                  const cellappmgr::InformCellLoad& msg) {
  // Find the peer by app_id — InformCellLoad carries the CellApp's own
  // app_id rather than re-advertising its address, so we index the
  // lookup linearly. CellApp counts are small (≤ 255) and load reports
  // arrive at a low cadence; the O(N) lookup is well within budget.
  for (auto& [addr, info] : cellapps_) {
    if (info.app_id == msg.app_id) {
      info.load = std::clamp(msg.load, 0.f, 1.f);
      info.entity_count = msg.entity_count;
      info.last_load_report_at = Clock::now();
      // Push the fresh load into each Space partition where this
      // CellApp owns a Cell. Balance consumes from CellInfo.load.
      for (auto& [_, partition] : spaces_) {
        for (auto* ci : partition.bsp.Leaves()) {
          if (ci->cellapp_addr == addr) {
            auto* mut = partition.bsp.FindCellByIdMutable(ci->cell_id);
            if (mut != nullptr) mut->load = info.load;
          }
        }
      }
      return;
    }
  }
  ATLAS_LOG_WARNING("CellAppMgr: InformCellLoad for unknown app_id={}", msg.app_id);
}

void CellAppMgr::OnCreateSpaceRequest(const Address& src, Channel* ch,
                                      const cellappmgr::CreateSpaceRequest& msg) {
  // Review-fix S2/S3: always reply — success OR failure. The originator
  // (BaseApp / script) tracks a per-request callback via request_id and
  // needs a terminal signal to resolve it.
  auto send_reply = [&](bool ok, cellappmgr::CellID cell_id, Address host_addr) {
    cellappmgr::SpaceCreatedResult reply;
    reply.request_id = msg.request_id;
    reply.space_id = msg.space_id;
    reply.success = ok;
    reply.cell_id = cell_id;
    reply.host_addr = host_addr;
    // Prefer reply_addr (BaseApp's advertised RUDP) over the raw src
    // because BaseApp may have multiple channels into CellAppMgr; the
    // reply_addr is the one its pending-requests table is keyed on.
    // Fall back to `ch` if reply_addr is default-constructed.
    if (msg.reply_addr.Port() != 0) {
      auto reply_ch = Network().ConnectRudpNocwnd(msg.reply_addr);
      if (reply_ch) (void)(*reply_ch)->SendMessage(reply);
    } else if (ch != nullptr) {
      (void)ch->SendMessage(reply);
    }
    (void)src;
  };

  if (msg.space_id == kInvalidSpaceID) {
    ATLAS_LOG_WARNING("CellAppMgr: CreateSpaceRequest with invalid space_id=0");
    send_reply(/*ok=*/false, 0, Address{});
    return;
  }
  if (spaces_.contains(msg.space_id)) {
    ATLAS_LOG_WARNING("CellAppMgr: CreateSpaceRequest for existing space_id={}", msg.space_id);
    send_reply(/*ok=*/false, 0, Address{});
    return;
  }

  const auto* host = PickHostForNewSpace();
  if (host == nullptr) {
    ATLAS_LOG_ERROR("CellAppMgr: CreateSpaceRequest space_id={} — no CellApps available to host",
                    msg.space_id);
    send_reply(/*ok=*/false, 0, Address{});
    return;
  }

  // Seed a single-cell BSP (whole-space leaf) hosted entirely on the
  // chosen CellApp. Later splits happen when more CellApps join and
  // rebalance decisions start making sense.
  const cellappmgr::CellID cell_id = next_cell_id_++;
  CellInfo leaf;
  leaf.cell_id = cell_id;
  leaf.cellapp_addr = host->internal_addr;
  leaf.load = host->load;
  leaf.entity_count = 0;

  SpacePartition partition;
  partition.space_id = msg.space_id;
  partition.bsp.InitSingleCell(leaf);
  spaces_.emplace(msg.space_id, std::move(partition));

  // Tell the host CellApp about its new Cell and push the geometry so
  // its OffloadChecker + GhostMaintainer have the data they need.
  SendAddCell(*host, msg.space_id, cell_id, CellBounds{});
  BroadcastGeometry(spaces_[msg.space_id]);

  ATLAS_LOG_INFO("CellAppMgr: created Space {} on CellApp app_id={} ({}:{}), cell_id={}",
                 msg.space_id, host->app_id, host->internal_addr.Ip(), host->internal_addr.Port(),
                 cell_id);
  send_reply(/*ok=*/true, cell_id, host->internal_addr);
}

void CellAppMgr::OnCellAppDeath(const Address& internal_addr) {
  auto it = cellapps_.find(internal_addr);
  if (it == cellapps_.end()) return;
  const uint32_t app_id = it->second.app_id;
  cellapps_.erase(it);
  // For PR-5 we simply log the orphaned cells. Full rehoming is deferred.
  for (const auto& [space_id, partition] : spaces_) {
    for (const auto* ci : partition.bsp.Leaves()) {
      if (ci->cellapp_addr == internal_addr) {
        ATLAS_LOG_ERROR(
            "CellAppMgr: CellApp app_id={} died — space_id={} cell_id={} is now orphaned", app_id,
            space_id, ci->cell_id);
      }
    }
  }
}

// ============================================================================
// Tick / Load balance
// ============================================================================

void CellAppMgr::TickLoadBalance() {
  if (spaces_.empty()) return;
  for (auto& [space_id, partition] : spaces_) {
    partition.bsp.Balance(kBalanceSafetyBound);
    // Broadcast unconditionally. The bsp_blob is tens of bytes; skipping
    // sends when the tree didn't move would require diffing which costs
    // more than the redundant bytes for a ≤255-peer cluster. Revisit if
    // profiling says otherwise.
    BroadcastGeometry(partition);
    (void)space_id;
  }
}

// ============================================================================
// Helpers
// ============================================================================

auto CellAppMgr::PickHostForNewSpace() const -> const CellAppInfo* {
  // Least-loaded first; ties broken by lowest app_id for determinism.
  const CellAppInfo* best = nullptr;
  for (const auto& [_, info] : cellapps_) {
    if (best == nullptr) {
      best = &info;
      continue;
    }
    if (info.load < best->load || (info.load == best->load && info.app_id < best->app_id)) {
      best = &info;
    }
  }
  return best;
}

void CellAppMgr::SendAddCell(const CellAppInfo& target, SpaceID space_id,
                             cellappmgr::CellID cell_id, const CellBounds& bounds) {
  if (target.channel == nullptr) {
    ATLAS_LOG_WARNING("CellAppMgr: AddCellToSpace skipped — no channel to app_id={}",
                      target.app_id);
    return;
  }
  cellappmgr::AddCellToSpace msg;
  msg.space_id = space_id;
  msg.cell_id = cell_id;
  msg.bounds = bounds;
  (void)target.channel->SendMessage(msg);
}

void CellAppMgr::BroadcastGeometry(const SpacePartition& partition) {
  // Serialize the tree once, then fan it out to every peer hosting a
  // Cell in this Space. Peers outside the Space don't care — shipping
  // geometry to them would just burn bandwidth.
  BinaryWriter w;
  partition.bsp.Serialize(w);
  const auto blob = w.Detach();

  cellappmgr::UpdateGeometry msg;
  msg.space_id = partition.space_id;
  msg.bsp_blob.assign(blob.begin(), blob.end());

  // Build the set of hosting addresses via the tree's leaf list.
  std::unordered_map<Address, bool> hosts;
  for (const auto* ci : partition.bsp.Leaves()) hosts[ci->cellapp_addr] = true;

  for (const auto& [addr, _] : hosts) {
    auto it = cellapps_.find(addr);
    if (it == cellapps_.end() || it->second.channel == nullptr) continue;
    (void)it->second.channel->SendMessage(msg);
  }
}

}  // namespace atlas
