#include "cellappmgr.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>

#include "baseapp/baseapp_messages.h"
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

// Mirror of BaseAppMgr's helper - rewrites a 0-IP advertised address to
// the packet's actual source so peers behind NAT / on loopback still end
// up reachable.
auto ResolveAdvertisedAddr(const Address& advertised, const Address& src) -> Address {
  if (advertised.Ip() != 0) return advertised;
  return Address(src.Ip(), advertised.Port());
}

}  // namespace

auto CellAppMgr::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher("cellappmgr");
  NetworkInterface network(dispatcher);
  CellAppMgr app(dispatcher, network);
  return app.RunApp(argc, argv);
}

CellAppMgr::CellAppMgr(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network) {}

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

  // Subscribe to CellApp death notifications so we can rehome BSP
  // leaves onto surviving CellApps and announce the death to BaseApps
  // so they restore their Reals from cached backups.
  GetMachinedClient().Subscribe(
      machined::ListenerType::kDeath, ProcessType::kCellApp, nullptr,
      [this](const machined::DeathNotification& n) { OnCellAppDeath(n.internal_addr); });

  // Track BaseApps directly - the death broadcast fans out to every
  // BaseApp. Direct path keeps CellAppMgr's cross-process surface small
  // (no new CellAppMgr<->BaseAppMgr channel).
  GetMachinedClient().Subscribe(
      machined::ListenerType::kBoth, ProcessType::kBaseApp,
      [this](const machined::BirthNotification& n) {
        auto ch = Network().ConnectRudpNocwnd(n.internal_addr);
        if (ch) {
          baseapps_.insert_or_assign(n.internal_addr, static_cast<Channel*>(*ch));
          ATLAS_LOG_INFO("CellAppMgr: BaseApp born at {}:{}", n.internal_addr.Ip(),
                         n.internal_addr.Port());
        }
      },
      [this](const machined::DeathNotification& n) {
        if (baseapps_.erase(n.internal_addr) > 0) {
          ATLAS_LOG_INFO("CellAppMgr: BaseApp died at {}:{}", n.internal_addr.Ip(),
                         n.internal_addr.Port());
        }
      });

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

void CellAppMgr::OnRegisterCellApp(const Address& src, Channel* ch,
                                   const cellappmgr::RegisterCellApp& msg) {
  const Address kInternalAddr = ResolveAdvertisedAddr(msg.internal_addr, src);

  if (cellapps_.contains(kInternalAddr)) {
    ATLAS_LOG_WARNING("CellAppMgr: duplicate CellApp registration for {}:{}", kInternalAddr.Ip(),
                      kInternalAddr.Port());
    cellappmgr::RegisterCellAppAck ack;
    ack.success = false;
    if (ch != nullptr) {
      if (auto r = ch->SendMessage(ack); !r) {
        ATLAS_LOG_WARNING("CellAppMgr: rejection ack send failed to {}: {}",
                          kInternalAddr.ToString(), r.Error().Message());
      }
    }
    return;
  }

  // app_id lives in the entity_id high byte, so we cap allocations at
  // 255. If we exhaust that pool the cluster has grown past what the
  // current EntityID layout supports and the fix is a wider scheme,
  // not a work-around here.
  if (next_cellapp_app_id_ > kMaxCellAppAppId) {
    ATLAS_LOG_ERROR(
        "CellAppMgr: CellApp app_id pool exhausted (> {}) — rejecting register from {}:{}",
        kMaxCellAppAppId, kInternalAddr.Ip(), kInternalAddr.Port());
    cellappmgr::RegisterCellAppAck ack;
    ack.success = false;
    if (ch != nullptr) {
      if (auto r = ch->SendMessage(ack); !r) {
        ATLAS_LOG_WARNING("CellAppMgr: pool-exhausted rejection ack send failed to {}: {}",
                          kInternalAddr.ToString(), r.Error().Message());
      }
    }
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
  if (ch != nullptr) {
    if (auto r = ch->SendMessage(ack); !r) {
      // Cellapp blocks until it sees this ack - drop => orphaned cellapp
      // until the registry's reconnect loop fires.
      ATLAS_LOG_WARNING("CellAppMgr: success-ack send failed to {} (app_id={}): {}",
                        kInternalAddr.ToString(), app_id, r.Error().Message());
    }
  }

  ATLAS_LOG_INFO("CellAppMgr: CellApp registered app_id={} internal={}:{}", app_id,
                 kInternalAddr.Ip(), kInternalAddr.Port());
}

void CellAppMgr::OnInformCellLoad(const Address& /*src*/, Channel* /*ch*/,
                                  const cellappmgr::InformCellLoad& msg) {
  // Find the peer by app_id - InformCellLoad carries the CellApp's own
  // app_id rather than re-advertising its address, so we index the
  // lookup linearly. CellApp counts are small (<= 255) and load reports
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
  // Always reply - success OR failure. The originator (BaseApp /
  // script) tracks a per-request callback via request_id and needs a
  // terminal signal to resolve it.
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
      if (reply_ch) {
        if (auto r = (*reply_ch)->SendMessage(reply); !r) {
          ATLAS_LOG_WARNING(
              "CellAppMgr: SpaceCreatedResult send failed (space_id={} via reply_addr {}): {}",
              reply.space_id, msg.reply_addr.ToString(), r.Error().Message());
        }
      }
    } else if (ch != nullptr) {
      if (auto r = ch->SendMessage(reply); !r) {
        ATLAS_LOG_WARNING("CellAppMgr: SpaceCreatedResult send failed (space_id={}, src {}): {}",
                          reply.space_id, src.ToString(), r.Error().Message());
      }
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
  const uint32_t dead_app_id = it->second.app_id;
  cellapps_.erase(it);

  // All Real entities hosted on the dead CellApp are lost (BaseApp
  // restores them from backup - orthogonal to mgr-side routing). Our
  // job here is purely to re-point every BSP leaf owned by the dead
  // app onto a surviving CellApp so future CreateCellEntity / Offload
  // traffic for that cell lands somewhere reachable.
  if (cellapps_.empty()) {
    ATLAS_LOG_CRITICAL(
        "CellAppMgr: CellApp app_id={} died and no survivors remain — all "
        "BSP leaves orphaned until a new CellApp registers",
        dead_app_id);
    return;
  }

  // Per-space {new_host} pairs collected during rehoming - the
  // BaseApp side uses this to look up where each of its entities'
  // cells went. We record ONE new_host per Space (the first
  // successful reassignment); multi-cell spaces still work because
  // each dead leaf is individually repointed above, but BaseApps
  // target Reals by space, not by cell. If a Space had multiple dead
  // cells split across different survivors, BaseApp may need to
  // re-check; for the typical 1 cell per space case this is exact.
  std::vector<std::pair<SpaceID, Address>> rehomes;

  for (auto& [space_id, partition] : spaces_) {
    bool reassigned_any = false;
    Address first_new_host{};
    for (auto* leaf : partition.bsp.LeavesMutable()) {
      if (leaf->cellapp_addr != internal_addr) continue;

      const auto* alt = PickAlternateHost(internal_addr);
      if (alt == nullptr) {
        // PickAlternateHost only returns nullptr when cellapps_ is
        // empty, which we already guarded above. Defensive log for a
        // would-be regression.
        ATLAS_LOG_ERROR(
            "CellAppMgr: rehoming cell_id={} (space {}) failed — no "
            "alternate host; leaf left pointing at dead app",
            leaf->cell_id, space_id);
        break;
      }

      ATLAS_LOG_INFO(
          "CellAppMgr: rehoming cell_id={} (space {}) from dead app_id={} "
          "to survivor app_id={}",
          leaf->cell_id, space_id, dead_app_id, alt->app_id);

      leaf->cellapp_addr = alt->internal_addr;
      // Start the leaf's load from the new host's current load. Keeping
      // the dead app's last-known load would skew the next Balance pass.
      leaf->load = alt->load;

      // Tell the new host to materialise the local Cell. UpdateGeometry
      // alone wouldn't - OnUpdateGeometry only resizes existing Cells,
      // never creates them (cellapp.cc:1141).
      SendAddCell(*alt, space_id, leaf->cell_id, leaf->bounds);
      reassigned_any = true;
      if (first_new_host.Ip() == 0) first_new_host = alt->internal_addr;
    }

    if (reassigned_any) {
      // Tell every CellApp hosting ANY leaf in this Space that the
      // layout changed. BroadcastGeometry reads the (now-updated) leaf
      // list, so the new host is included in the fan-out and learns
      // about the full BSP in one pass.
      BroadcastGeometry(partition);
      rehomes.emplace_back(space_id, first_new_host);
    }
  }

  // Tell every BaseApp about the death so they restore Reals from
  // backup onto the new hosts. Direct path - see the Init subscribe
  // comment.
  if (!baseapps_.empty()) {
    baseapp::CellAppDeath notify;
    notify.dead_addr = internal_addr;
    notify.rehomes = std::move(rehomes);
    for (const auto& [addr, ch] : baseapps_) {
      if (ch != nullptr) (void)ch->SendMessage(notify);
    }
  }
}

void CellAppMgr::TickLoadBalance() {
  if (spaces_.empty()) return;
  for (auto& [space_id, partition] : spaces_) {
    partition.bsp.Balance(kBalanceSafetyBound);
    BroadcastGeometry(partition);
    (void)space_id;
  }
}

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

auto CellAppMgr::PickAlternateHost(const Address& exclude_addr) const -> const CellAppInfo* {
  // Prefer a survivor on a different machine; otherwise choose least-loaded.
  const CellAppInfo* best_diff_ip = nullptr;
  const CellAppInfo* best_any = nullptr;
  for (const auto& [addr, info] : cellapps_) {
    const bool diff_ip = (addr.Ip() != exclude_addr.Ip());
    auto is_better = [](const CellAppInfo* a, const CellAppInfo* b) {
      if (a == nullptr) return true;
      if (b->load != a->load) return b->load < a->load;
      return b->app_id < a->app_id;
    };
    if (is_better(best_any, &info)) best_any = &info;
    if (diff_ip && is_better(best_diff_ip, &info)) best_diff_ip = &info;
  }
  return best_diff_ip != nullptr ? best_diff_ip : best_any;
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

void CellAppMgr::BroadcastGeometry(SpacePartition& partition) {
  // Serialize the tree once, then fan it out to every peer hosting a
  // Cell in this Space. Peers outside the Space don't care - shipping
  // geometry to them would just burn bandwidth.
  BinaryWriter w;
  partition.bsp.Serialize(w);
  auto blob = w.Detach();

  // Short-circuit when the serialised bytes haven't changed since the
  // last fan-out. Steady-state balanced clusters can tick Balance()
  // without actually moving a split line, so without this every Space
  // re-ships the same tree at TickLoadBalance cadence - wasted bandwidth
  // that scales with peer x space count.
  if (blob == partition.last_broadcast_blob) return;

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

  partition.last_broadcast_blob = std::move(blob);
}

}  // namespace atlas
