#include "dbappmgr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#include "foundation/log.h"
#include "network/channel.h"
#include "network/machined_types.h"
#include "server/server_app_option.h"
#include "server/watcher.h"

namespace atlas {

namespace {

constexpr DatabaseID kFirstShardDbid = 1;
constexpr DatabaseID kShardHighLimit = std::numeric_limits<DatabaseID>::max();

auto ShardSpan(const dbappmgr::ShardEntry& entry) -> uint64_t {
  return static_cast<uint64_t>(entry.high_dbid - entry.low_dbid);
}

auto SameOwner(const dbappmgr::ShardEntry& a, const dbappmgr::ShardEntry& b) -> bool {
  return a.dbapp_id == b.dbapp_id && a.dbapp_addr == b.dbapp_addr && a.is_retiring == b.is_retiring;
}

auto ShardOverlaps(const dbappmgr::ShardEntry& a, const dbappmgr::ShardEntry& b) -> bool {
  return a.low_dbid < b.high_dbid && b.low_dbid < a.high_dbid;
}

}  // namespace

auto DBAppMgr::Run(int argc, char* argv[]) -> int {
  EventDispatcher dispatcher;
  NetworkInterface network(dispatcher);
  DBAppMgr app(dispatcher, network);
  return app.RunApp(argc, argv);
}

DBAppMgr::DBAppMgr(EventDispatcher& dispatcher, NetworkInterface& network)
    : ManagerApp(dispatcher, network) {}

auto DBAppMgr::Init(int argc, char* argv[]) -> bool {
  if (!ManagerApp::Init(argc, argv)) return false;

  auto& table = Network().InterfaceTable();
  (void)table.RegisterTypedHandler<dbappmgr::RegisterDbApp>(
      [this](const Address& src, Channel* ch, const dbappmgr::RegisterDbApp& msg) {
        OnRegisterDbApp(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<dbappmgr::InformLoad>(
      [this](const Address& src, Channel* ch, const dbappmgr::InformLoad& msg) {
        OnInformLoad(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<dbappmgr::GetShardTable>(
      [this](const Address& src, Channel* ch, const dbappmgr::GetShardTable& msg) {
        OnGetShardTable(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<dbappmgr::RecoverDBAppState>(
      [this](const Address& src, Channel* ch, const dbappmgr::RecoverDBAppState& msg) {
        OnRecoverDBAppState(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<dbappmgr::HealthProbe>(
      [this](const Address& src, Channel* ch, const dbappmgr::HealthProbe& msg) {
        OnHealthProbe(src, ch, msg);
      });

  GetMachinedClient().Subscribe(
      machined::ListenerType::kDeath, ProcessType::kDbApp, nullptr,
      [this](const machined::DeathNotification& n) { OnDbAppDeath(n.internal_addr, n.reason); });

  recovery_deadline_ = startup_recovery_window_ > Duration::zero()
                           ? Clock::now() + startup_recovery_window_
                           : TimePoint{};

  ATLAS_LOG_INFO("DBAppMgr: initialised");
  return true;
}

void DBAppMgr::RegisterWatchers() {
  ManagerApp::RegisterWatchers();
  auto& wr = GetWatcherRegistry();
  wr.Add<std::size_t>("dbappmgr/dbapp_count",
                      std::function<std::size_t()>([this] { return dbapps_.size(); }));
  wr.Add<std::size_t>("dbappmgr/shards/count",
                      std::function<std::size_t()>([this] { return shard_table_.size(); }));
  wr.Add<uint32_t>("dbappmgr/shards/version",
                   std::function<uint32_t()>([this] { return shard_table_version_; }));
  wr.Add<std::string>("dbappmgr/shards/table",
                      std::function<std::string()>([this] { return ShardTableSummary(); }));
  wr.Add<bool>("dbappmgr/ha/recovery_window_active",
               std::function<bool()>([this] { return RecoveryWindowActive(); }));
  wr.Add<std::string>("dbappmgr/ha/recovery_window_status", std::function<std::string()>([this] {
                        const bool active = RecoveryWindowActive();
                        const auto remaining =
                            active ? recovery_deadline_ - Clock::now() : Duration::zero();
                        const auto remaining_ms = std::max<int64_t>(
                            0, std::chrono::duration_cast<Milliseconds>(remaining).count());
                        return std::format("state={} active={} remaining_ms={} shards={}",
                                           active ? "closed" : "open", active ? 1 : 0, remaining_ms,
                                           shard_table_.size());
                      }));
}

void DBAppMgr::OnRegisterDbApp(const Address& src, Channel* ch,
                               const dbappmgr::RegisterDbApp& msg) {
  const Address kInternalAddr = ResolveAdvertisedAddr(msg.internal_addr, src);
  dbappmgr::RegisterDbAppAck ack;

  if (kInternalAddr.Port() == 0) {
    ack.success = false;
    if (ch != nullptr) (void)ch->SendMessage(ack);
    return;
  }

  if (dbapps_.contains(kInternalAddr)) {
    ATLAS_LOG_WARNING("DBAppMgr: duplicate DBApp registration for {}:{}", kInternalAddr.Ip(),
                      kInternalAddr.Port());
    ack.success = false;
    if (ch != nullptr) (void)ch->SendMessage(ack);
    return;
  }

  const uint32_t kAppId = AllocateAppId(msg.known_app_id);
  DBAppInfo info;
  info.internal_addr = kInternalAddr;
  info.app_id = kAppId;
  info.channel = ch;
  info.registered_at = Clock::now();
  const auto [it, inserted] = dbapps_.emplace(kInternalAddr, std::move(info));
  if (!inserted) {
    ack.success = false;
    if (ch != nullptr) (void)ch->SendMessage(ack);
    return;
  }
  app_id_index_.emplace(kAppId, it->first);

  if (!ShouldDeferShardAssignment(msg)) AssignInitialShard(kAppId, kInternalAddr);
  if (ch != nullptr) shard_table_subscribers_.insert_or_assign(kInternalAddr, ch);

  ack.success = true;
  ack.dbapp_id = kAppId;
  ack.shard_table_version = shard_table_version_;
  ack.entries = shard_table_;
  if (ch != nullptr) (void)ch->SendMessage(ack);

  ATLAS_LOG_INFO("DBAppMgr: DBApp registered app_id={} internal={}:{} shards={} version={}", kAppId,
                 kInternalAddr.Ip(), kInternalAddr.Port(), shard_table_.size(),
                 shard_table_version_);
}

void DBAppMgr::OnRecoverDBAppState(const Address& src, Channel* ch,
                                   const dbappmgr::RecoverDBAppState& msg) {
  DBAppInfo* info = FindDbAppByAppId(msg.dbapp_id);
  if (info == nullptr) {
    ATLAS_LOG_WARNING("DBAppMgr: RecoverDBAppState for unknown dbapp_id={} from {}:{}",
                      msg.dbapp_id, src.Ip(), src.Port());
    return;
  }
  if (!MatchesRegisteredSource(*info, src, ch, "RecoverDBAppState")) return;
  if (msg.shard_table_version < shard_table_version_) {
    ATLAS_LOG_WARNING("DBAppMgr: ignoring stale RecoverDBAppState app_id={} version={} current={}",
                      msg.dbapp_id, msg.shard_table_version, shard_table_version_);
    return;
  }

  std::vector<dbappmgr::ShardEntry> recovered;
  recovered.reserve(msg.shards.size());
  for (const auto& shard : msg.shards) {
    if (shard.dbapp_id != msg.dbapp_id) {
      ATLAS_LOG_WARNING("DBAppMgr: RecoverDBAppState app_id={} contains shard for app_id={}",
                        msg.dbapp_id, shard.dbapp_id);
      continue;
    }
    auto entry = shard;
    entry.dbapp_addr = info->internal_addr;
    recovered.push_back(entry);
  }
  if (recovered.empty()) return;

  shard_table_.erase(
      std::remove_if(
          shard_table_.begin(), shard_table_.end(),
          [&](const auto& entry) {
            if (entry.dbapp_id == msg.dbapp_id || entry.dbapp_addr == info->internal_addr) {
              return true;
            }
            return std::any_of(recovered.begin(), recovered.end(),
                               [&](const auto& shard) { return ShardOverlaps(entry, shard); });
          }),
      shard_table_.end());
  shard_table_.insert(shard_table_.end(), recovered.begin(), recovered.end());
  NormalizeShardTable();
  shard_table_version_ = std::max(shard_table_version_, msg.shard_table_version);
  BroadcastShardTableUpdate();

  ATLAS_LOG_INFO("DBAppMgr: recovered app_id={} shards={} version={}", msg.dbapp_id,
                 recovered.size(), shard_table_version_);
}

void DBAppMgr::OnInformLoad(const Address& src, Channel* ch, const dbappmgr::InformLoad& msg) {
  DBAppInfo* info = FindDbAppByAppId(msg.dbapp_id);
  if (info == nullptr) {
    ATLAS_LOG_WARNING("DBAppMgr: InformLoad for unknown dbapp_id={} from {}:{}", msg.dbapp_id,
                      src.Ip(), src.Port());
    return;
  }
  if (!MatchesRegisteredSource(*info, src, ch, "InformLoad")) return;

  info->load = std::isfinite(msg.load) ? std::clamp(msg.load, 0.0f, 1.0f) : 1.0f;
  info->entity_count = msg.entity_count;
  info->pending_checkout_count = msg.pending_checkout_count;
  info->write_queue_depth = msg.write_queue_depth;
  info->last_load_report_at = Clock::now();
}

void DBAppMgr::OnGetShardTable(const Address& src, Channel* ch,
                               const dbappmgr::GetShardTable& msg) {
  if (ch == nullptr) return;
  shard_table_subscribers_.insert_or_assign(src, ch);
  auto response = BuildShardTableResponse(msg.request_id, msg.known_version);
  (void)ch->SendMessage(response);
}

void DBAppMgr::OnHealthProbe(const Address& src, Channel* ch, const dbappmgr::HealthProbe& msg) {
  if (ch == nullptr) return;
  dbappmgr::HealthProbeAck ack;
  ack.nonce = msg.nonce;
  ack.game_time = GameTime();
  ack.is_active_reviver =
      reviver_subject_.RecordPingAndIsActive(src, msg.reviver_priority, Clock::now());
  (void)ch->SendMessage(ack);
}

void DBAppMgr::OnDbAppDeath(const Address& internal_addr, uint8_t reason) {
  auto it = dbapps_.find(internal_addr);
  if (it == dbapps_.end()) return;

  const uint32_t kDeadAppId = it->second.app_id;
  if (reason == 0) {
    ATLAS_LOG_INFO("DBAppMgr: DBApp app_id={} deregistered ({}:{})", kDeadAppId, internal_addr.Ip(),
                   internal_addr.Port());
  } else {
    ATLAS_LOG_WARNING("DBAppMgr: DBApp app_id={} died ({}:{})", kDeadAppId, internal_addr.Ip(),
                      internal_addr.Port());
  }
  app_id_index_.erase(kDeadAppId);
  shard_table_subscribers_.erase(internal_addr);
  dbapps_.erase(it);
  ReassignShards(kDeadAppId);
}

auto DBAppMgr::FindShard(DatabaseID dbid) const -> std::optional<dbappmgr::ShardEntry> {
  if (dbid <= kInvalidDBID) return std::nullopt;
  const auto it = std::find_if(shard_table_.begin(), shard_table_.end(), [dbid](const auto& entry) {
    return entry.low_dbid <= dbid && dbid < entry.high_dbid;
  });
  if (it == shard_table_.end()) return std::nullopt;
  return *it;
}

auto DBAppMgr::ShardTableSummary() const -> std::string {
  std::string out;
  for (const auto& entry : shard_table_) {
    if (!out.empty()) out += ";";
    out += std::format("[{},{})=>{}@{}:{}", entry.low_dbid, entry.high_dbid, entry.dbapp_id,
                       entry.dbapp_addr.Ip(), entry.dbapp_addr.Port());
  }
  return out;
}

auto DBAppMgr::FindDbAppByAppId(uint32_t app_id) -> DBAppInfo* {
  const auto kIndexIt = app_id_index_.find(app_id);
  if (kIndexIt == app_id_index_.end()) return nullptr;
  const auto kIt = dbapps_.find(kIndexIt->second);
  if (kIt == dbapps_.end()) {
    app_id_index_.erase(kIndexIt);
    return nullptr;
  }
  return &kIt->second;
}

auto DBAppMgr::FindDbAppByAppId(uint32_t app_id) const -> const DBAppInfo* {
  const auto kIndexIt = app_id_index_.find(app_id);
  if (kIndexIt == app_id_index_.end()) return nullptr;
  const auto kIt = dbapps_.find(kIndexIt->second);
  return (kIt != dbapps_.end()) ? &kIt->second : nullptr;
}

auto DBAppMgr::MatchesRegisteredSource(const DBAppInfo& info, const Address& src, const Channel* ch,
                                       std::string_view operation) const -> bool {
  if (src != info.internal_addr) {
    ATLAS_LOG_WARNING("DBAppMgr: {} source mismatch for app_id={} expected {}:{} got {}:{}",
                      operation, info.app_id, info.internal_addr.Ip(), info.internal_addr.Port(),
                      src.Ip(), src.Port());
    return false;
  }
  if (ch != nullptr && info.channel != nullptr && info.channel != ch) {
    ATLAS_LOG_WARNING("DBAppMgr: {} channel mismatch for app_id={}", operation, info.app_id);
    return false;
  }
  return true;
}

auto DBAppMgr::FindLeastLoaded() const -> const DBAppInfo* {
  const DBAppInfo* best = nullptr;
  for (const auto& [addr, info] : dbapps_) {
    (void)addr;
    if (info.is_retiring) continue;
    if (best == nullptr || IsBetterCandidate(info, *best)) best = &info;
  }
  return best;
}

auto DBAppMgr::IsBetterCandidate(const DBAppInfo& candidate, const DBAppInfo& incumbent) -> bool {
  if (candidate.load != incumbent.load) return candidate.load < incumbent.load;
  if (candidate.pending_checkout_count != incumbent.pending_checkout_count) {
    return candidate.pending_checkout_count < incumbent.pending_checkout_count;
  }
  if (candidate.write_queue_depth != incumbent.write_queue_depth) {
    return candidate.write_queue_depth < incumbent.write_queue_depth;
  }
  if (candidate.entity_count != incumbent.entity_count) {
    return candidate.entity_count < incumbent.entity_count;
  }
  return candidate.app_id < incumbent.app_id;
}

auto DBAppMgr::ResolveAdvertisedAddr(const Address& advertised, const Address& src) -> Address {
  if (advertised.Ip() != 0) return advertised;
  return Address(src.Ip(), advertised.Port());
}

auto DBAppMgr::AllocateAppId(uint32_t known_app_id) -> uint32_t {
  if (known_app_id != 0 && !app_id_index_.contains(known_app_id)) {
    next_app_id_ = std::max(next_app_id_, known_app_id + 1);
    return known_app_id;
  }
  return next_app_id_++;
}

void DBAppMgr::AssignInitialShard(uint32_t app_id, const Address& addr) {
  if (shard_table_.empty()) {
    shard_table_.push_back(dbappmgr::ShardEntry{kFirstShardDbid, kShardHighLimit, app_id, addr});
    BumpShardTableVersion();
    return;
  }
  SplitLargestShardFor(app_id, addr);
}

void DBAppMgr::SplitLargestShardFor(uint32_t app_id, const Address& addr) {
  auto it =
      std::max_element(shard_table_.begin(), shard_table_.end(), [](const auto& a, const auto& b) {
        if (ShardSpan(a) != ShardSpan(b)) return ShardSpan(a) < ShardSpan(b);
        return a.low_dbid > b.low_dbid;
      });
  if (it == shard_table_.end() || ShardSpan(*it) < 2) return;

  const DatabaseID kSplit = it->low_dbid + static_cast<DatabaseID>(ShardSpan(*it) / 2);
  const dbappmgr::ShardEntry kNewEntry{kSplit, it->high_dbid, app_id, addr};
  it->high_dbid = kSplit;
  shard_table_.push_back(kNewEntry);
  NormalizeShardTable();
  BumpShardTableVersion();
}

auto DBAppMgr::RecoveryWindowActive() const -> bool {
  return recovery_deadline_ != TimePoint{} && Clock::now() < recovery_deadline_;
}

auto DBAppMgr::ShouldDeferShardAssignment(const dbappmgr::RegisterDbApp& msg) const -> bool {
  return RecoveryWindowActive() && msg.known_app_id != 0 && msg.known_shard_table_version != 0;
}

void DBAppMgr::ReassignShards(uint32_t dead_app_id) {
  bool changed = false;
  const DBAppInfo* target = FindLeastLoaded();
  if (target == nullptr) {
    const auto kOldSize = shard_table_.size();
    shard_table_.erase(
        std::remove_if(shard_table_.begin(), shard_table_.end(),
                       [dead_app_id](const auto& entry) { return entry.dbapp_id == dead_app_id; }),
        shard_table_.end());
    changed = shard_table_.size() != kOldSize;
  } else {
    for (auto& entry : shard_table_) {
      if (entry.dbapp_id != dead_app_id) continue;
      entry.dbapp_id = target->app_id;
      entry.dbapp_addr = target->internal_addr;
      entry.is_retiring = false;
      changed = true;
    }
  }

  if (!changed) return;
  NormalizeShardTable();
  BumpShardTableVersion();
}

void DBAppMgr::NormalizeShardTable() {
  std::sort(shard_table_.begin(), shard_table_.end(),
            [](const auto& a, const auto& b) { return a.low_dbid < b.low_dbid; });
  std::vector<dbappmgr::ShardEntry> normalized;
  normalized.reserve(shard_table_.size());
  for (const auto& entry : shard_table_) {
    if (entry.low_dbid >= entry.high_dbid) continue;
    if (!normalized.empty() && normalized.back().high_dbid == entry.low_dbid &&
        SameOwner(normalized.back(), entry)) {
      normalized.back().high_dbid = entry.high_dbid;
      continue;
    }
    normalized.push_back(entry);
  }
  shard_table_ = std::move(normalized);
}

void DBAppMgr::BumpShardTableVersion() {
  if (shard_table_version_ == std::numeric_limits<uint32_t>::max()) {
    shard_table_version_ = 1;
  } else {
    ++shard_table_version_;
  }
  BroadcastShardTableUpdate();
}

void DBAppMgr::BroadcastShardTableUpdate() {
  if (shard_table_subscribers_.empty()) return;
  dbappmgr::ShardTableUpdate update;
  update.version = shard_table_version_;
  update.entries = shard_table_;

  for (auto it = shard_table_subscribers_.begin(); it != shard_table_subscribers_.end();) {
    Channel* ch = it->second;
    if (ch == nullptr || ch->IsCondemned()) {
      it = shard_table_subscribers_.erase(it);
      continue;
    }
    if (!ch->SendMessage(update).HasValue()) {
      it = shard_table_subscribers_.erase(it);
      continue;
    }
    ++it;
  }
}

auto DBAppMgr::BuildShardTableResponse(uint32_t request_id, uint32_t known_version) const
    -> dbappmgr::ShardTableResponse {
  dbappmgr::ShardTableResponse response;
  response.request_id = request_id;
  response.version = shard_table_version_;
  if (known_version != shard_table_version_) response.entries = shard_table_;
  return response;
}

}  // namespace atlas
