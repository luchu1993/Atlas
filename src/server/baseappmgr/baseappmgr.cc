#include "baseappmgr.h"

#include <algorithm>
#include <format>

#include "foundation/clock.h"
#include "foundation/log.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "server/watcher.h"

namespace atlas {

void BaseAppMgr::DbidAffinityTable::Remember(DatabaseID dbid, uint32_t app_id, TimePoint now) {
  if (dbid == kInvalidDBID || app_id == 0) {
    return;
  }

  if (auto existing = entries_.find(dbid); existing != entries_.end()) {
    if (existing->second.app_id != app_id) {
      auto reverse = dbids_by_app_.find(existing->second.app_id);
      if (reverse != dbids_by_app_.end()) {
        reverse->second.erase(dbid);
        if (reverse->second.empty()) {
          dbids_by_app_.erase(reverse);
        }
      }
    }
  }

  entries_[dbid] = Entry{app_id, now};
  dbids_by_app_[app_id].insert(dbid);
}

void BaseAppMgr::DbidAffinityTable::Erase(DatabaseID dbid) {
  auto it = entries_.find(dbid);
  if (it == entries_.end()) {
    return;
  }

  if (auto reverse = dbids_by_app_.find(it->second.app_id); reverse != dbids_by_app_.end()) {
    reverse->second.erase(dbid);
    if (reverse->second.empty()) {
      dbids_by_app_.erase(reverse);
    }
  }

  entries_.erase(it);
}

void BaseAppMgr::DbidAffinityTable::ForgetApp(uint32_t app_id) {
  auto reverse = dbids_by_app_.find(app_id);
  if (reverse == dbids_by_app_.end()) {
    return;
  }

  for (DatabaseID dbid : reverse->second) {
    entries_.erase(dbid);
  }
  dbids_by_app_.erase(reverse);
}

void BaseAppMgr::DbidAffinityTable::PruneExpired(TimePoint now, Duration ttl) {
  for (auto it = entries_.begin(); it != entries_.end();) {
    if (now - it->second.last_assigned_at <= ttl) {
      ++it;
      continue;
    }

    if (auto reverse = dbids_by_app_.find(it->second.app_id); reverse != dbids_by_app_.end()) {
      reverse->second.erase(it->first);
      if (reverse->second.empty()) {
        dbids_by_app_.erase(reverse);
      }
    }
    it = entries_.erase(it);
  }
}

auto BaseAppMgr::DbidAffinityTable::Find(DatabaseID dbid) const -> std::optional<Entry> {
  auto it = entries_.find(dbid);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  return it->second;
}

auto BaseAppMgr::BaseAppInfo::QueuePressure() const -> float {
  const float kPressureUnits =
      static_cast<float>(pending_prepare_count + pending_force_logoff_count + deferred_login_count +
                         logoff_in_flight_count) +
      static_cast<float>(detached_proxy_count) * 0.1f;

  // Queue depth is a balancing hint, not a hard overload signal. Scale it
  // conservatively so transient login bursts spread across BaseApps instead
  // of collapsing the whole cluster into "no_baseapp" rejections.
  return std::min(0.35f, kPressureUnits / 512.0f);
}

auto BaseAppMgr::BaseAppInfo::IsHardOverloaded(float overload_threshold) const -> bool {
  return measured_load >= overload_threshold ||
         pending_prepare_count >= BaseAppMgr::kHardOverloadPendingPrepareLimit ||
         deferred_login_count >= BaseAppMgr::kHardOverloadDeferredLoginLimit ||
         (pending_force_logoff_count + logoff_in_flight_count) >=
             BaseAppMgr::kHardOverloadLogoffLimit;
}

void BaseAppMgr::BaseAppInfo::ApplyLoadReport(
    float load, uint32_t reported_entity_count, uint32_t reported_proxy_count,
    uint32_t reported_pending_prepare_count, uint32_t reported_pending_force_logoff_count,
    uint32_t reported_detached_proxy_count, uint32_t reported_logoff_in_flight_count,
    uint32_t reported_deferred_login_count, TimePoint now) {
  measured_load = std::clamp(load, 0.0f, 1.0f);
  entity_count = reported_entity_count;
  proxy_count = reported_proxy_count;
  pending_prepare_count = reported_pending_prepare_count;
  pending_force_logoff_count = reported_pending_force_logoff_count;
  detached_proxy_count = reported_detached_proxy_count;
  logoff_in_flight_count = reported_logoff_in_flight_count;
  deferred_login_count = reported_deferred_login_count;
  pending_login_allocations = 0;
  effective_load = std::clamp(std::max(measured_load, QueuePressure()), 0.0f, 1.0f);
  last_load_report_at = now;
}

void BaseAppMgr::BaseAppInfo::ReserveLoginSlot(float load_increment) {
  ++pending_login_allocations;
  ++pending_prepare_count;
  ++entity_count;
  ++proxy_count;
  effective_load = std::min(1.0f, std::max(effective_load + load_increment, QueuePressure()));
}

auto BaseAppMgr::BaseAppInfo::HasFreshLoad(TimePoint now, Duration stale_after) const -> bool {
  if (last_load_report_at == TimePoint{}) {
    return true;
  }

  return (now - last_load_report_at) <= stale_after;
}

namespace {

auto ResolveAdvertisedAddr(const Address& advertised, const Address& src) -> Address {
  if (advertised.Ip() != 0) {
    return advertised;
  }
  return Address(src.Ip(), advertised.Port());
}

}  // namespace

// ============================================================================
// run — static entry point
// ============================================================================

auto BaseAppMgr::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);
  BaseAppMgr app(dispatcher, network);
  return app.RunApp(argc, argv);
}

BaseAppMgr::BaseAppMgr(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network) {}

// ============================================================================
// init
// ============================================================================

auto BaseAppMgr::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  auto& table = Network().InterfaceTable();

  (void)table.RegisterTypedHandler<baseappmgr::RegisterBaseApp>(
      [this](const Address& src, Channel* ch, const baseappmgr::RegisterBaseApp& msg) {
        OnRegisterBaseapp(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::BaseAppReady>(
      [this](const Address& src, Channel* ch, const baseappmgr::BaseAppReady& msg) {
        OnBaseappReady(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::InformLoad>(
      [this](const Address& src, Channel* ch, const baseappmgr::InformLoad& msg) {
        OnInformLoad(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<login::AllocateBaseApp>(
      [this](const Address& src, Channel* ch, const login::AllocateBaseApp& msg) {
        OnAllocateBaseapp(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::RegisterGlobalBase>(
      [this](const Address& src, Channel* ch, const baseappmgr::RegisterGlobalBase& msg) {
        OnRegisterGlobalBase(src, ch, msg);
      });

  (void)table.RegisterTypedHandler<baseappmgr::DeregisterGlobalBase>(
      [this](const Address& src, Channel* ch, const baseappmgr::DeregisterGlobalBase& msg) {
        OnDeregisterGlobalBase(src, ch, msg);
      });

  // Subscribe to BaseApp death notifications
  GetMachinedClient().Subscribe(
      machined::ListenerType::kDeath, ProcessType::kBaseApp, nullptr,
      [this](const machined::DeathNotification& n) { OnBaseappDeath(n.internal_addr); });

  ATLAS_LOG_INFO("BaseAppMgr: initialised");
  return true;
}

void BaseAppMgr::Fini() {
  ManagerApp::Fini();
}

// ============================================================================
// register_watchers
// ============================================================================

void BaseAppMgr::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<std::size_t>("baseappmgr/baseapp_count",
                      std::function<std::size_t()>([this] { return baseapps_.size(); }));
  wr.Add<std::size_t>("baseappmgr/global_base_count",
                      std::function<std::size_t()>([this] { return global_bases_.size(); }));
  wr.Add<std::size_t>("baseappmgr/dbid_affinity_count",
                      std::function<std::size_t()>([this] { return dbid_affinity_.size(); }));
}

// ============================================================================
// Message handlers
// ============================================================================

void BaseAppMgr::OnRegisterBaseapp(const Address& src, Channel* ch,
                                   const baseappmgr::RegisterBaseApp& msg) {
  const Address kInternalAddr = ResolveAdvertisedAddr(msg.internal_addr, src);
  const Address kExternalAddr = ResolveAdvertisedAddr(msg.external_addr, src);

  if (baseapps_.contains(kInternalAddr)) {
    ATLAS_LOG_WARNING("BaseAppMgr: duplicate BaseApp registration for internal addr {}:{}",
                      kInternalAddr.Ip(), kInternalAddr.Port());

    baseappmgr::RegisterBaseAppAck ack;
    ack.success = false;
    (void)ch->SendMessage(ack);
    return;
  }

  uint32_t app_id = next_app_id_++;
  BaseAppInfo info;
  info.internal_addr = kInternalAddr;
  info.external_addr = kExternalAddr;
  info.app_id = app_id;
  info.channel = ch;
  const auto [it, inserted] = baseapps_.emplace(kInternalAddr, std::move(info));
  if (!inserted) {
    ATLAS_LOG_ERROR("BaseAppMgr: failed to insert BaseApp registration for {}:{}",
                    kInternalAddr.Ip(), kInternalAddr.Port());
    baseappmgr::RegisterBaseAppAck ack;
    ack.success = false;
    (void)ch->SendMessage(ack);
    return;
  }
  app_id_index_.emplace(app_id, it->first);

  baseappmgr::RegisterBaseAppAck ack;
  ack.success = true;
  ack.app_id = app_id;
  ack.game_time = GameTime();
  (void)ch->SendMessage(ack);

  ATLAS_LOG_INFO("BaseAppMgr: BaseApp registered app_id={} internal={}:{} external={}:{}", app_id,
                 kInternalAddr.Ip(), kInternalAddr.Port(), kExternalAddr.Ip(),
                 kExternalAddr.Port());
}

void BaseAppMgr::OnBaseappReady(const Address& src, Channel* ch,
                                const baseappmgr::BaseAppReady& msg) {
  BaseAppInfo* info = FindBaseappByAppId(msg.app_id);
  if (info == nullptr) {
    ATLAS_LOG_WARNING("BaseAppMgr: BaseAppReady for unknown app_id={} from {}:{}", msg.app_id,
                      src.Ip(), src.Port());
    return;
  }

  if (!MatchesRegisteredSource(*info, src, ch, "BaseAppReady")) return;

  info->is_ready = true;
  ATLAS_LOG_INFO("BaseAppMgr: BaseApp app_id={} is ready", msg.app_id);
}

void BaseAppMgr::OnInformLoad(const Address& src, Channel* ch, const baseappmgr::InformLoad& msg) {
  BaseAppInfo* info = FindBaseappByAppId(msg.app_id);
  if (info == nullptr) {
    ATLAS_LOG_WARNING("BaseAppMgr: InformLoad for unknown app_id={} from {}:{}", msg.app_id,
                      src.Ip(), src.Port());
    return;
  }

  if (!MatchesRegisteredSource(*info, src, ch, "InformLoad")) return;

  info->ApplyLoadReport(msg.load, msg.entity_count, msg.proxy_count, msg.pending_prepare_count,
                        msg.pending_force_logoff_count, msg.detached_proxy_count,
                        msg.logoff_in_flight_count, msg.deferred_login_count, Clock::now());
}

void BaseAppMgr::OnAllocateBaseapp(const Address& src, Channel* ch,
                                   const login::AllocateBaseApp& msg) {
  ATLAS_LOG_DEBUG("BaseAppMgr: allocate request_id={} type_id={} dbid={} from {}:{}",
                  msg.request_id, msg.type_id, msg.dbid, src.Ip(), src.Port());
  login::AllocateBaseAppResult result;
  result.request_id = msg.request_id;

  if (IsOverloaded()) {
    ATLAS_LOG_WARNING("BaseAppMgr: overloaded, rejecting AllocateBaseApp req={}", msg.request_id);
    result.success = false;
    (void)ch->SendMessage(result);
    return;
  }

  auto* best = FindAllocationTarget(msg.dbid);
  if (!best) {
    ATLAS_LOG_WARNING("BaseAppMgr: no ready BaseApp available for req={}", msg.request_id);
    result.success = false;
    (void)ch->SendMessage(result);
    return;
  }

  result.success = true;
  result.internal_addr = best->internal_addr;
  result.external_addr = best->external_addr;
  const auto kSendResult = ch->SendMessage(result);
  if (!kSendResult) {
    ATLAS_LOG_WARNING("BaseAppMgr: failed to reply AllocateBaseApp req={} app_id={}: {}",
                      msg.request_id, best->app_id, kSendResult.Error().Message());
    return;
  }

  RecordSuccessfulAllocation(best->app_id, msg.dbid, Clock::now());

  ATLAS_LOG_DEBUG("BaseAppMgr: allocated BaseApp app_id={} for req={} dbid={}", best->app_id,
                  msg.request_id, msg.dbid);
  (void)src;
}

void BaseAppMgr::OnRegisterGlobalBase(const Address& src, Channel* /*ch*/,
                                      const baseappmgr::RegisterGlobalBase& msg) {
  GlobalBaseEntry entry;
  entry.key = msg.key;
  entry.base_addr = src;
  entry.entity_id = msg.entity_id;
  entry.type_id = msg.type_id;
  global_bases_[msg.key] = std::move(entry);

  baseappmgr::GlobalBaseNotification notif;
  notif.key = msg.key;
  notif.base_addr = src;
  notif.entity_id = msg.entity_id;
  notif.type_id = msg.type_id;
  notif.added = true;
  BroadcastToAllBaseapps(notif);

  ATLAS_LOG_INFO("BaseAppMgr: global base '{}' registered entity={}", msg.key, msg.entity_id);
}

void BaseAppMgr::OnDeregisterGlobalBase(const Address& /*src*/, Channel* /*ch*/,
                                        const baseappmgr::DeregisterGlobalBase& msg) {
  auto it = global_bases_.find(msg.key);
  if (it == global_bases_.end()) return;

  baseappmgr::GlobalBaseNotification notif;
  notif.key = msg.key;
  notif.base_addr = it->second.base_addr;
  notif.entity_id = it->second.entity_id;
  notif.type_id = it->second.type_id;
  notif.added = false;
  global_bases_.erase(it);
  BroadcastToAllBaseapps(notif);

  ATLAS_LOG_INFO("BaseAppMgr: global base '{}' deregistered", msg.key);
}

// ============================================================================
// Internal helpers
// ============================================================================

auto BaseAppMgr::FindBaseappByAppId(uint32_t app_id) -> BaseAppInfo* {
  const auto kIndexIt = app_id_index_.find(app_id);
  if (kIndexIt == app_id_index_.end()) return nullptr;

  const auto kIt = baseapps_.find(kIndexIt->second);
  if (kIt == baseapps_.end()) {
    app_id_index_.erase(kIndexIt);
    return nullptr;
  }

  return &kIt->second;
}

auto BaseAppMgr::FindBaseappByAppId(uint32_t app_id) const -> const BaseAppInfo* {
  const auto kIndexIt = app_id_index_.find(app_id);
  if (kIndexIt == app_id_index_.end()) return nullptr;

  const auto kIt = baseapps_.find(kIndexIt->second);
  return (kIt != baseapps_.end()) ? &kIt->second : nullptr;
}

auto BaseAppMgr::MatchesRegisteredSource(const BaseAppInfo& info, const Address& src,
                                         const Channel* ch, std::string_view operation) const
    -> bool {
  if (src != info.internal_addr) {
    ATLAS_LOG_WARNING("BaseAppMgr: {} source mismatch for app_id={} expected {}:{} got {}:{}",
                      operation, info.app_id, info.internal_addr.Ip(), info.internal_addr.Port(),
                      src.Ip(), src.Port());
    return false;
  }

  if (ch != nullptr && info.channel != nullptr && info.channel != ch) {
    ATLAS_LOG_WARNING("BaseAppMgr: {} channel mismatch for app_id={}", operation, info.app_id);
    return false;
  }

  return true;
}

auto BaseAppMgr::IsAllocationCandidate(const BaseAppInfo& info, TimePoint now,
                                       Duration stale_after) const -> bool {
  return info.is_ready && !info.is_retiring && info.HasFreshLoad(now, stale_after);
}

auto BaseAppMgr::IsBetterCandidate(const BaseAppInfo& candidate, const BaseAppInfo& incumbent)
    -> bool {
  if (candidate.effective_load != incumbent.effective_load) {
    return candidate.effective_load < incumbent.effective_load;
  }

  if (candidate.proxy_count != incumbent.proxy_count) {
    return candidate.proxy_count < incumbent.proxy_count;
  }

  if (candidate.entity_count != incumbent.entity_count) {
    return candidate.entity_count < incumbent.entity_count;
  }

  return candidate.app_id < incumbent.app_id;
}

auto BaseAppMgr::LoadReportStaleAfter() const -> Duration {
  const int kUpdateHertz = std::max(Config().update_hertz, 1);
  const auto kExpectedTick =
      std::chrono::duration_cast<Duration>(std::chrono::duration<double>(1.0 / kUpdateHertz));
  const auto kMinimumStaleness = std::chrono::duration_cast<Duration>(std::chrono::seconds(1));
  return std::max(kExpectedTick * 10, kMinimumStaleness);
}

auto BaseAppMgr::FindLeastLoaded() const -> const BaseAppInfo* {
  const BaseAppInfo* best = nullptr;
  const auto kNow = Clock::now();
  const auto kStaleAfter = LoadReportStaleAfter();
  for (const auto& [addr, info] : baseapps_) {
    if (!IsAllocationCandidate(info, kNow, kStaleAfter)) {
      continue;
    }

    if (best == nullptr || IsBetterCandidate(info, *best)) {
      best = &info;
    }
  }
  return best;
}

auto BaseAppMgr::ShouldPreferAffinity(const BaseAppInfo& preferred,
                                      const BaseAppInfo* least_loaded) const -> bool {
  if (preferred.IsHardOverloaded(kOverloadThreshold)) {
    return false;
  }

  if (least_loaded == nullptr || preferred.app_id == least_loaded->app_id) {
    return true;
  }

  // Preserve DBID affinity using reported process load rather than the
  // queue-biased balancing score. For shortline relogin storms the preferred
  // BaseApp often carries more detached proxies precisely because it can
  // complete the reconnect locally; routing away from it defeats the fast
  // path and amplifies force-logoff pressure.
  const float kAllowedLoad = std::min(1.0f, least_loaded->measured_load + kDbidAffinityLoadSlack);
  return preferred.measured_load <= kAllowedLoad;
}

auto BaseAppMgr::FindAllocationTarget(DatabaseID dbid) -> const BaseAppInfo* {
  const auto kNow = Clock::now();
  const auto kStaleAfter = LoadReportStaleAfter();
  dbid_affinity_.PruneExpired(kNow, kDbidAffinityTtl);

  const BaseAppInfo* least_loaded = FindLeastLoaded();
  if (dbid == kInvalidDBID) {
    return least_loaded;
  }

  const auto kAffinity = dbid_affinity_.Find(dbid);
  if (!kAffinity) {
    return least_loaded;
  }

  const auto* preferred = FindBaseappByAppId(kAffinity->app_id);
  if (preferred == nullptr || !IsAllocationCandidate(*preferred, kNow, kStaleAfter)) {
    dbid_affinity_.Erase(dbid);
    return least_loaded;
  }

  if (ShouldPreferAffinity(*preferred, least_loaded)) {
    return preferred;
  }

  return least_loaded;
}

void BaseAppMgr::RecordSuccessfulAllocation(uint32_t app_id, DatabaseID dbid, TimePoint now) {
  if (auto* reserved = FindBaseappByAppId(app_id)) {
    reserved->ReserveLoginSlot(kLoginAllocationLoadIncrement);
    dbid_affinity_.Remember(dbid, reserved->app_id, now);
  }
}

auto BaseAppMgr::IsOverloaded() const -> bool {
  const auto* best = FindLeastLoaded();
  if (!best || !best->IsHardOverloaded(kOverloadThreshold)) {
    overload_start_ = {};
    logins_since_overload_ = 0;
    return false;
  }

  auto now = Clock::now();
  if (overload_start_ == TimePoint{}) {
    overload_start_ = now;
    logins_since_overload_ = 0;
  }

  auto duration = now - overload_start_;
  if (duration > std::chrono::seconds(5) || logins_since_overload_ >= kOverloadLoginLimit) {
    return true;
  }

  ++logins_since_overload_;
  return false;
}

void BaseAppMgr::BroadcastToAllBaseapps(const baseappmgr::GlobalBaseNotification& notif) {
  for (auto& [addr, info] : baseapps_) {
    if (info.is_ready && info.channel) {
      if (auto r = info.channel->SendMessage(notif); !r) {
        // A missed notification leaves peers permanently disagreeing on
        // which BaseApp owns a singleton (e.g. ChatService) — RPCs to
        // that mailbox land on stale or absent owner.  No automatic
        // resync; operator must restart or rely on death detection.
        ATLAS_LOG_ERROR(
            "BaseAppMgr: GlobalBaseNotification dropped to baseapp {}: {} "
            "— peer-view divergence on global mailbox",
            addr.ToString(), r.Error().Message());
      }
    }
  }
}

void BaseAppMgr::OnBaseappDeath(const Address& addr) {
  auto it = baseapps_.find(addr);
  if (it == baseapps_.end()) return;
  ATLAS_LOG_WARNING("BaseAppMgr: BaseApp app_id={} died ({}:{})", it->second.app_id, addr.Ip(),
                    addr.Port());
  dbid_affinity_.ForgetApp(it->second.app_id);
  app_id_index_.erase(it->second.app_id);
  baseapps_.erase(it);

  // Clean up any global bases owned by the dead BaseApp
  for (auto git = global_bases_.begin(); git != global_bases_.end();) {
    if (git->second.base_addr == addr) {
      baseappmgr::GlobalBaseNotification notif;
      notif.key = git->second.key;
      notif.base_addr = addr;
      notif.entity_id = git->second.entity_id;
      notif.type_id = git->second.type_id;
      notif.added = false;
      BroadcastToAllBaseapps(notif);
      git = global_bases_.erase(git);
    } else {
      ++git;
    }
  }
}

}  // namespace atlas
