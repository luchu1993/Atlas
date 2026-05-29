#include "cellappmgr.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "baseapp/baseapp_messages.h"
#include "foundation/log.h"
#include "network/channel.h"
#include "network/event_dispatcher.h"
#include "network/interface_table.h"
#include "network/machined_types.h"
#include "network/network_interface.h"
#include "network/reliable_udp.h"
#include "platform/filesystem.h"
#include "serialization/binary_stream.h"
#include "server/machined_client.h"
#include "server/server_app_option.h"
#include "server/watcher.h"

namespace atlas {

namespace {

ServerAppOption<float> s_lb_tick_load_weight{
    1.0f, "cellappmgr_lb_tick_load_weight", "cellappmgr/lb/weights/tick_load",
    WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_witness_weight{
    0.02f, "cellappmgr_lb_witness_weight", "cellappmgr/lb/weights/witness",
    WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_aoi_peer_weight{
    0.001f, "cellappmgr_lb_aoi_peer_weight", "cellappmgr/lb/weights/aoi_peer",
    WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_aoi_reliable_mib_weight{
    1.0f, "cellappmgr_lb_aoi_reliable_mib_weight", "cellappmgr/lb/weights/aoi_reliable_mib",
    WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_aoi_unreliable_mib_weight{
    0.5f, "cellappmgr_lb_aoi_unreliable_mib_weight",
    "cellappmgr/lb/weights/aoi_unreliable_mib", WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_backup_mib_weight{
    0.1f, "cellappmgr_lb_backup_mib_weight", "cellappmgr/lb/weights/backup_mib",
    WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_auto_split_load_threshold{
    0.85f, "cellappmgr_lb_auto_split_load_threshold",
    "cellappmgr/lb/auto_split/load_threshold", WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_auto_split_idle_load_threshold{
    0.35f, "cellappmgr_lb_auto_split_idle_load_threshold",
    "cellappmgr/lb/auto_split/idle_load_threshold", WatcherMode::kReadWrite};
ServerAppOption<uint32_t> s_lb_auto_split_sustain_ticks{
    3u, "cellappmgr_lb_auto_split_sustain_ticks",
    "cellappmgr/lb/auto_split/sustain_balance_ticks", WatcherMode::kReadWrite};
ServerAppOption<float> s_lb_auto_merge_load_threshold{
    0.10f, "cellappmgr_lb_auto_merge_load_threshold",
    "cellappmgr/lb/auto_merge/load_threshold", WatcherMode::kReadWrite};
ServerAppOption<uint32_t> s_lb_auto_merge_sustain_ticks{
    3u, "cellappmgr_lb_auto_merge_sustain_ticks",
    "cellappmgr/lb/auto_merge/sustain_balance_ticks", WatcherMode::kReadWrite};
ServerAppOption<uint32_t> s_lb_retire_drain_watchdog_ms{
    30000u, "cellappmgr_lb_retire_drain_watchdog_ms",
    "cellappmgr/lb/retire/drain_watchdog_ms", WatcherMode::kReadWrite};
ServerAppOption<uint32_t> s_lb_load_report_stale_ms{
    3000u, "cellappmgr_lb_load_report_stale_ms",
    "cellappmgr/lb/load_report_stale_ms", WatcherMode::kReadWrite};
auto NonNegative(float v) -> float {
  return std::isfinite(v) && v > 0.f ? v : 0.f;
}

auto DurationMs(Duration duration) -> int64_t {
  return std::chrono::duration_cast<Milliseconds>(duration).count();
}

auto AgeMsSince(TimePoint t) -> int64_t {
  if (t.time_since_epoch() == Duration::zero()) return -1;
  return std::max<int64_t>(0, DurationMs(Clock::now() - t));
}

auto RetireDrainWatchdogWindow() -> Duration {
  return Milliseconds{s_lb_retire_drain_watchdog_ms.Value()};
}

auto LoadReportStaleWindow() -> Duration {
  return Milliseconds{s_lb_load_report_stale_ms.Value()};
}

auto LastLoadReportAt(const CellAppMgr::CellAppInfo& info) -> TimePoint {
  if (info.last_load_report_at.time_since_epoch() != Duration::zero()) {
    return info.last_load_report_at;
  }
  return info.registered_at;
}

auto IsLoadReportStale(const CellAppMgr::CellAppInfo& info, TimePoint now) -> bool {
  return now - LastLoadReportAt(info) >= LoadReportStaleWindow();
}

auto HasFreshLoadReport(const CellAppMgr::CellAppInfo& info, TimePoint now) -> bool {
  return !IsLoadReportStale(info, now);
}

auto IsAssignableForLb(const CellAppMgr::CellAppInfo& info, TimePoint now) -> bool {
  return !info.is_retiring && HasFreshLoadReport(info, now);
}

auto MiB(uint64_t bytes) -> float {
  return static_cast<float>(static_cast<double>(bytes) / 1048576.0);
}

auto WeightedCellLoad(const cellappmgr::InformCellLoad::CellReport& rep,
                      float fallback_tick_load) -> float {
  const float tick_load = NonNegative(rep.tick_load) > 0.f ? rep.tick_load : fallback_tick_load;
  const float weighted =
      NonNegative(s_lb_tick_load_weight.Value()) * NonNegative(tick_load) +
      NonNegative(s_lb_witness_weight.Value()) * static_cast<float>(rep.witness_count) +
      NonNegative(s_lb_aoi_peer_weight.Value()) * static_cast<float>(rep.aoi_peer_count) +
      NonNegative(s_lb_aoi_reliable_mib_weight.Value()) * MiB(rep.aoi_reliable_bytes) +
      NonNegative(s_lb_aoi_unreliable_mib_weight.Value()) * MiB(rep.aoi_unreliable_bytes) +
      NonNegative(s_lb_backup_mib_weight.Value()) * MiB(rep.backup_bytes);
  return std::max(NonNegative(tick_load), NonNegative(weighted));
}

// Mirrors BaseAppMgr's NAT/loopback fix for 0-IP advertised addresses.
auto ResolveAdvertisedAddr(const Address& advertised, const Address& src) -> Address {
  if (advertised.Ip() != 0) return advertised;
  return Address(src.Ip(), advertised.Port());
}

auto MidCoord(float lo, float hi) -> float {
  if (std::isfinite(lo) && std::isfinite(hi)) return (lo + hi) * 0.5f;
  if (std::isfinite(lo)) return lo + 1.f;
  if (std::isfinite(hi)) return hi - 1.f;
  return 0.f;
}

auto ClampInsideCoord(float value, float lo, float hi) -> float {
  if (std::isfinite(lo) && std::isfinite(hi) && lo < hi) {
    const float pad = std::max(1e-3f, (hi - lo) * 1e-4f);
    return std::clamp(value, lo + pad, hi - pad);
  }
  if (std::isfinite(lo)) value = std::max(value, lo + 1e-3f);
  if (std::isfinite(hi)) value = std::min(value, hi - 1e-3f);
  return value;
}

auto MidpointForAxis(const CellBounds& bounds, BSPAxis axis) -> float {
  return axis == BSPAxis::kX ? MidCoord(bounds.min_x, bounds.max_x)
                             : MidCoord(bounds.min_z, bounds.max_z);
}

auto ClampInsideAxis(float value, const CellBounds& bounds, BSPAxis axis) -> float {
  return axis == BSPAxis::kX ? ClampInsideCoord(value, bounds.min_x, bounds.max_x)
                             : ClampInsideCoord(value, bounds.min_z, bounds.max_z);
}

using LoadBuckets =
    std::array<uint32_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount>;
using LoadCostBuckets =
    std::array<uint64_t, cellappmgr::InformCellLoad::CellReport::kLoadBucketCount>;

auto BucketTotal(const LoadCostBuckets& buckets) -> uint64_t {
  uint64_t total = 0;
  for (uint64_t bucket : buckets) total += bucket;
  return total;
}

auto BucketWeights(const LoadBuckets& count_buckets,
                   const LoadCostBuckets& load_buckets) -> LoadCostBuckets {
  if (BucketTotal(load_buckets) > 0) return load_buckets;

  LoadCostBuckets weights{};
  for (std::size_t i = 0; i < weights.size(); ++i) weights[i] = count_buckets[i];
  return weights;
}

auto BucketSplitPosition(const LoadCostBuckets& buckets, const CellBounds& bounds, BSPAxis axis)
    -> std::optional<float> {
  const float lo = axis == BSPAxis::kX ? bounds.min_x : bounds.min_z;
  const float hi = axis == BSPAxis::kX ? bounds.max_x : bounds.max_z;
  if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) return std::nullopt;

  const uint64_t total = BucketTotal(buckets);
  if (total == 0) return std::nullopt;

  uint64_t left = 0;
  uint64_t best_diff = std::numeric_limits<uint64_t>::max();
  double best_center_distance = std::numeric_limits<double>::max();
  std::optional<float> best;
  const float midpoint = MidCoord(lo, hi);
  for (std::size_t boundary = 1; boundary < buckets.size(); ++boundary) {
    left += buckets[boundary - 1];
    const uint64_t right = total - left;
    if (left == 0 || right == 0) continue;
    const uint64_t diff = left > right ? left - right : right - left;
    const float pos = lo + (hi - lo) * static_cast<float>(boundary) /
                               static_cast<float>(buckets.size());
    const double center_distance = std::abs(static_cast<double>(pos - midpoint));
    if (diff < best_diff ||
        (diff == best_diff && center_distance < best_center_distance)) {
      best_diff = diff;
      best_center_distance = center_distance;
      best = pos;
    }
  }
  return best;
}

auto FormatBuckets(const LoadBuckets& buckets) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    if (i > 0) out += ",";
    out += std::format("{}", buckets[i]);
  }
  out += "]";
  return out;
}

auto FormatBuckets(const LoadCostBuckets& buckets) -> std::string {
  std::string out = "[";
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    if (i > 0) out += ",";
    out += std::format("{}", buckets[i]);
  }
  out += "]";
  return out;
}

auto BoundsMidpoint(const CellBounds& bounds) -> std::pair<float, float> {
  return {MidCoord(bounds.min_x, bounds.max_x), MidCoord(bounds.min_z, bounds.max_z)};
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
  (void)table.RegisterTypedHandler<cellappmgr::AddCellToSpaceAck>(
      [this](const Address& src, Channel* ch, const cellappmgr::AddCellToSpaceAck& msg) {
        OnAddCellToSpaceAck(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::HealthProbe>(
      [this](const Address& src, Channel* ch, const cellappmgr::HealthProbe& msg) {
        OnHealthProbe(src, ch, msg);
      });
  (void)table.RegisterTypedHandler<cellappmgr::RecoverCellAppState>(
      [this](const Address& src, Channel* ch, const cellappmgr::RecoverCellAppState& msg) {
        OnRecoverCellAppState(src, ch, msg);
      });

  // CellApp deaths rehome orphaned leaves and tell BaseApps to restore Reals.
  GetMachinedClient().Subscribe(
      machined::ListenerType::kDeath, ProcessType::kCellApp, nullptr,
      [this](const machined::DeathNotification& n) { OnCellAppDeath(n.internal_addr, n.reason); });

  // Direct BaseApp tracking avoids a CellAppMgr-to-BaseAppMgr channel.
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

  // Hold the restore gate for the startup window so live CellApps can report
  // their held BSP (OnRecoverCellAppState) before we balance against a partial
  // view — even when there is no snapshot to seed reattach state.
  recovery_deadline_ = startup_quiescence_window_ > Duration::zero()
                           ? Clock::now() + startup_quiescence_window_
                           : TimePoint{};

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
  wr.Add<std::size_t>("cellappmgr/lb/pending_geometry_broadcasts",
                      std::function<std::size_t()>(
                          [this] { return pending_geometry_broadcasts_.size(); }));
  wr.Add<std::size_t>("cellappmgr/lb/pending_space_creates",
                      std::function<std::size_t()>(
                          [this] { return pending_space_creates_awaiting_cellapps_.size(); }));
  wr.Add<std::string>("cellappmgr/lb/pending_space_create_status",
                      std::function<std::string()>(
                          [this] { return BuildPendingSpaceCreateSummary(); }));
  wr.Add<std::size_t>("cellappmgr/lb/load_report_stale_count",
                      std::function<std::size_t()>(
                          [this] { return StaleLoadReportCount(); }));
  wr.Add<std::string>("cellappmgr/lb/cellapps",
                      std::function<std::string()>(
                          [this] { return BuildCellAppLoadSummary(); }));
  wr.Add<std::string>("cellappmgr/lb/spaces",
                      std::function<std::string()>(
                          [this] { return BuildSpaceLoadSummary(); }));
  wr.Add<std::string>("cellappmgr/lb/last_decision",
                      std::function<std::string()>(
                          [this] { return BuildLbDecisionSummary(); }));
  wr.Add<std::string>("cellappmgr/lb/decision_history",
                      std::function<std::string()>(
                          [this] { return BuildLbDecisionHistorySummary(); }));
  wr.Add<uint64_t>("cellappmgr/lb/decision_count",
                   std::function<uint64_t()>(
                       [this] { return lb_decision_count_; }));
  wr.Add<std::size_t>("cellappmgr/lb/decision_history_size",
                      std::function<std::size_t()>(
                          [this] { return lb_decision_history_.size(); }));
  wr.Add<std::size_t>("cellappmgr/lb/retire/count",
                      std::function<std::size_t()>(
                          [this] { return RetiringCellAppCount(); }));
  wr.Add<std::size_t>("cellappmgr/lb/retire/drain_count",
                      std::function<std::size_t()>(
                          [this] { return RetireDrainCount(); }));
  wr.Add<std::size_t>("cellappmgr/lb/retire/stuck_count",
                      std::function<std::size_t()>(
                          [this] { return RetireStuckDrainCount(); }));
  wr.Add<std::string>("cellappmgr/lb/retire/status",
                      std::function<std::string()>(
                          [this] { return BuildRetireStatusSummary(); }));
  wr.AddRw<uint32_t>("cellappmgr/lb/retire/app_id",
                     std::function<uint32_t()>(
                         [this] { return RetiringAppIdForWatcher(); }),
                     std::function<bool(uint32_t)>(
                         [this](uint32_t app_id) { return SetRetiringAppId(app_id); }));
  wr.Add<bool>("cellappmgr/ha/recovery_window_active",
               std::function<bool()>([this] { return RecoveryWindowActive(); }));
  wr.Add<std::string>("cellappmgr/ha/recovery_window_status",
                      std::function<std::string()>([this] {
                        const bool active = RecoveryWindowActive();
                        const int64_t remaining_ms =
                            active ? std::max<int64_t>(
                                         0, DurationMs(recovery_deadline_ - Clock::now()))
                                   : 0;
                        return std::format(
                            "state={} active={} remaining_ms={} pending_geometry={}",
                            active ? "closed" : "open", active ? 1 : 0, remaining_ms,
                            pending_geometry_broadcasts_.size());
                      }));
}

void CellAppMgr::OnTickComplete() {
  ManagerApp::OnTickComplete();
  DrainPendingGeometryBroadcasts();
  AuditRetireDrainWatchdog();
  DrainExpiredCreateSpaceRequests();
  const auto tick = GameTime();
  if (tick - last_balance_tick_ >= kBalanceTickInterval) {
    last_balance_tick_ = tick;
    TickLoadBalance();
  }
}

auto CellAppMgr::AppIdForAddress(const Address& addr) const -> uint32_t {
  auto it = cellapps_.find(addr);
  return it == cellapps_.end() ? 0 : it->second.app_id;
}

void CellAppMgr::RecordLbDecision(std::string action, std::string reason, SpaceID space_id,
                                  cellappmgr::CellID cell_id,
                                  cellappmgr::CellID target_cell_id,
                                  uint32_t source_app_id, uint32_t target_app_id,
                                  uint64_t geometry_version, std::string detail) {
  ++lb_decision_count_;
  last_lb_decision_ = LbDecision{lb_decision_count_, GameTime(), std::move(action),
                                 std::move(reason),   space_id,    cell_id,
                                 target_cell_id,      source_app_id,
                                 target_app_id,       geometry_version,
                                 std::move(detail)};
  lb_decision_history_.push_back(last_lb_decision_);
  if (lb_decision_history_.size() > kLbDecisionHistoryLimit) {
    lb_decision_history_.erase(lb_decision_history_.begin());
  }
}

void CellAppMgr::OnRegisterCellApp(const Address& src, Channel* ch,
                                   const cellappmgr::RegisterCellApp& msg) {
  const Address kInternalAddr = ResolveAdvertisedAddr(msg.internal_addr, src);

  if (auto existing_it = cellapps_.find(kInternalAddr); existing_it != cellapps_.end()) {
    auto& existing = existing_it->second;
    if (existing.channel != nullptr && existing.channel != ch) {
      ATLAS_LOG_WARNING("CellAppMgr: duplicate CellApp registration for {}:{}",
                        kInternalAddr.Ip(), kInternalAddr.Port());
      SendRegisterCellAppAck(ch, kInternalAddr, /*app_id=*/0, /*success=*/false, "duplicate");
      return;
    }
    existing.channel = ch;
    existing.registered_at = Clock::now();
    // Idempotent re-register: the worker re-reports its held BSP via
    // RecoverCellAppState right after this ack, so the mgr needs no replay.
    SendRegisterCellAppAck(ch, kInternalAddr, existing.app_id, /*success=*/true, "re-register");
    ATLAS_LOG_INFO("CellAppMgr: CellApp re-registered app_id={} internal={}:{}",
                   existing.app_id, kInternalAddr.Ip(), kInternalAddr.Port());
    return;
  }

  const auto app_id_in_use = [this](uint32_t id) {
    return std::any_of(cellapps_.begin(), cellapps_.end(),
                       [id](const auto& e) { return e.second.app_id == id; });
  };
  uint32_t app_id;
  const char* register_reason = "register";
  if (msg.known_app_id >= 1 && msg.known_app_id <= kMaxCellAppAppId &&
      !app_id_in_use(msg.known_app_id)) {
    // Snapshot-less recovery: the CellApp echoed an id it already owns; keep it
    // so its existing entities' EntityIDs (high byte = app_id) keep routing.
    app_id = msg.known_app_id;
    next_cellapp_app_id_ = std::max(next_cellapp_app_id_, static_cast<uint32_t>(app_id + 1));
    register_reason = "recover-app-id";
  } else {
    if (next_cellapp_app_id_ > kMaxCellAppAppId) {
      ATLAS_LOG_ERROR(
          "CellAppMgr: CellApp app_id pool exhausted (> {}) — rejecting register from {}:{}",
          kMaxCellAppAppId, kInternalAddr.Ip(), kInternalAddr.Port());
      SendRegisterCellAppAck(ch, kInternalAddr, /*app_id=*/0, /*success=*/false, "pool-exhausted");
      return;
    }
    app_id = next_cellapp_app_id_++;
  }
  CellAppInfo info;
  info.internal_addr = kInternalAddr;
  info.app_id = app_id;
  info.channel = ch;
  info.registered_at = Clock::now();
  cellapps_.emplace(kInternalAddr, std::move(info));

  SendRegisterCellAppAck(ch, kInternalAddr, app_id, /*success=*/true, register_reason);

  ATLAS_LOG_INFO("CellAppMgr: CellApp registered app_id={} internal={}:{} ({})", app_id,
                 kInternalAddr.Ip(), kInternalAddr.Port(), register_reason);

  const auto extended = Clock::now() + startup_quiescence_window_;
  for (auto& entry : pending_space_creates_awaiting_cellapps_) {
    entry.quiescence_deadline = extended;
  }

  // Elastic grow: existing Spaces split their heaviest leaf onto the new
  // cellapp; pending Spaces are handled by the deadline extension above.
  auto self_it = cellapps_.find(kInternalAddr);
  if (self_it != cellapps_.end()) GrowSpacesForNewCellApp(self_it->second);
}

void CellAppMgr::SendRegisterCellAppAck(Channel* ch, const Address& addr, uint32_t app_id,
                                        bool success, const char* context) {
  cellappmgr::RegisterCellAppAck ack;
  ack.success = success;
  ack.app_id = app_id;
  ack.game_time = GameTime();
  ack.tick_alignment_epoch_us = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(StartTime().time_since_epoch())
          .count());
  if (ch == nullptr) return;
  if (auto r = ch->SendMessage(ack); !r) {
    ATLAS_LOG_WARNING("CellAppMgr: {} register ack send failed to {} (app_id={}): {}",
                      context, addr.ToString(), app_id, r.Error().Message());
  }
}

void CellAppMgr::OnHealthProbe(const Address&, Channel* ch,
                               const cellappmgr::HealthProbe& msg) {
  if (ch == nullptr) return;
  cellappmgr::HealthProbeAck ack;
  ack.nonce = msg.nonce;
  ack.game_time = GameTime();
  (void)ch->SendMessage(ack);
}

void CellAppMgr::OnRecoverCellAppState(const Address& /*src*/, Channel* /*ch*/,
                                       const cellappmgr::RecoverCellAppState& msg) {
  // BigWorld-style recovery: a surviving CellApp reports the BSP it holds so a
  // mgr that lacks the topology (fresh start / revive) rebuilds from workers
  // instead of a snapshot file. Highest reported geometry_version wins; a
  // space is only rebuilt once every leaf owner is registered, so cell->app
  // routing resolves (typically the report from the last owner to rejoin).
  for (const auto& sg : msg.spaces) {
    if (sg.space_id == kInvalidSpaceID) continue;
    if (auto it = spaces_.find(sg.space_id);
        it != spaces_.end() && it->second.geometry_version >= sg.geometry_version) {
      continue;
    }
    BinaryReader r(std::span<const std::byte>(sg.bsp_blob.data(), sg.bsp_blob.size()));
    auto bsp = BSPTree::Deserialize(r);
    if (!bsp) {
      ATLAS_LOG_WARNING("CellAppMgr: recover bsp deserialize failed space={}: {}", sg.space_id,
                        bsp.Error().Message());
      continue;
    }
    const auto leaves = bsp->Leaves();
    if (leaves.empty() || bsp->FindCellById(bsp->PrimaryCellId()) == nullptr) continue;
    cellappmgr::CellID max_cell = 0;
    bool all_owners_known = true;
    for (const auto* leaf : leaves) {
      if (!cellapps_.contains(leaf->cellapp_addr)) {
        all_owners_known = false;
        break;
      }
      max_cell = std::max(max_cell, leaf->cell_id);
    }
    if (!all_owners_known) continue;
    SpacePartition partition;
    partition.space_id = sg.space_id;
    partition.bsp = std::move(*bsp);
    partition.geometry_version = sg.geometry_version;
    spaces_[sg.space_id] = std::move(partition);
    next_cell_id_ = std::max(next_cell_id_, static_cast<cellappmgr::CellID>(max_cell + 1));
    ATLAS_LOG_INFO("CellAppMgr: rebuilt space={} from worker report (version={}, leaves={})",
                   sg.space_id, sg.geometry_version, leaves.size());
  }
}

void CellAppMgr::OnInformCellLoad(const Address& src, Channel* ch,
                                  const cellappmgr::InformCellLoad& msg) {
  // Linear lookup by app_id; CellApp count is bounded at 255.
  for (auto& [addr, info] : cellapps_) {
    if (info.app_id != msg.app_id) continue;
    const bool has_sender_identity = ch != nullptr || src != Address{};
    const bool sender_matches = src == addr || (info.channel != nullptr && info.channel == ch);
    if (has_sender_identity && !sender_matches) {
      ATLAS_LOG_WARNING(
          "CellAppMgr: ignoring InformCellLoad from {} for app_id={} expected={}",
          src.ToString(), msg.app_id, addr.ToString());
      return;
    }
    info.load = std::clamp(msg.load, 0.f, 1.f);
    info.entity_count = msg.entity_count;
    info.last_load_report_at = Clock::now();
    if (msg.cells.empty()) {
      DrainExpiredCreateSpaceRequests();
      return;
    }
    const uint32_t total_entities = msg.entity_count;
    for (const auto& rep : msg.cells) {
      CellInfo* owned_cell = nullptr;
      const SpacePartition* owner_partition = nullptr;
      bool wrong_owner = false;
      for (auto& [_, partition] : spaces_) {
        auto* mut = partition.bsp.FindCellByIdMutable(rep.cell_id);
        if (mut == nullptr) continue;
        if (mut->cellapp_addr != addr) {
          wrong_owner = true;
          break;
        }
        owned_cell = mut;
        owner_partition = &partition;
        break;
      }
      if (owned_cell == nullptr) {
        if (wrong_owner) {
          if (HandleRetireDrainReport(addr, rep)) continue;
          ATLAS_LOG_WARNING("CellAppMgr: ignoring load for cell_id={} from non-owner app_id={}",
                            rep.cell_id, msg.app_id);
        }
        continue;
      }
      if (owner_partition != nullptr && rep.geometry_version != owner_partition->geometry_version) {
        ATLAS_LOG_DEBUG(
            "CellAppMgr: ignoring load for cell_id={} app_id={} geometry_version={} current={}",
            rep.cell_id, msg.app_id, rep.geometry_version, owner_partition->geometry_version);
        continue;
      }

      const float share =
          total_entities > 0
              ? static_cast<float>(rep.entity_count) / static_cast<float>(total_entities)
              : 1.f / std::max<float>(1.f, static_cast<float>(msg.cells.size()));
      const float fallback_tick_load = share * info.load;
      const float weighted_load = WeightedCellLoad(rep, fallback_tick_load);
      CellDistribution dist;
      dist.entity_count = rep.entity_count;
      dist.median_x = rep.median_x;
      dist.median_z = rep.median_z;
      dist.weighted_load = weighted_load;
      dist.tick_load = NonNegative(rep.tick_load) > 0.f ? rep.tick_load : fallback_tick_load;
      dist.script_tick_us = rep.script_tick_us;
      dist.native_tick_us = rep.native_tick_us;
      dist.witness_count = rep.witness_count;
      dist.aoi_peer_count = rep.aoi_peer_count;
      dist.aoi_reliable_bytes = rep.aoi_reliable_bytes;
      dist.aoi_unreliable_bytes = rep.aoi_unreliable_bytes;
      dist.backup_bytes = rep.backup_bytes;
      dist.x_buckets = rep.x_buckets;
      dist.z_buckets = rep.z_buckets;
      dist.x_load_buckets = rep.x_load_buckets;
      dist.z_load_buckets = rep.z_load_buckets;
      cell_distributions_[rep.cell_id] = dist;
      owned_cell->entity_count = rep.entity_count;
      owned_cell->tick_load = dist.tick_load;
      owned_cell->script_tick_us = rep.script_tick_us;
      owned_cell->native_tick_us = rep.native_tick_us;
      owned_cell->witness_count = rep.witness_count;
      owned_cell->aoi_peer_count = rep.aoi_peer_count;
      owned_cell->aoi_reliable_bytes = rep.aoi_reliable_bytes;
      owned_cell->aoi_unreliable_bytes = rep.aoi_unreliable_bytes;
      owned_cell->backup_bytes = rep.backup_bytes;
      owned_cell->x_buckets = rep.x_buckets;
      owned_cell->z_buckets = rep.z_buckets;
      owned_cell->x_load_buckets = rep.x_load_buckets;
      owned_cell->z_load_buckets = rep.z_load_buckets;
      owned_cell->load = weighted_load;
    }
    DrainExpiredCreateSpaceRequests();
    return;
  }
  ATLAS_LOG_WARNING("CellAppMgr: InformCellLoad for unknown app_id={}", msg.app_id);
}

void CellAppMgr::SendCreateSpaceReply(const cellappmgr::CreateSpaceRequest& msg, const Address& src,
                                      Channel* ch, bool ok, cellappmgr::CellID cell_id,
                                      Address host_addr) {
  cellappmgr::SpaceCreatedResult reply;
  reply.request_id = msg.request_id;
  reply.space_id = msg.space_id;
  reply.success = ok;
  reply.cell_id = cell_id;
  reply.host_addr = host_addr;
  // reply_addr keys BaseApp's pending-requests table; raw src is the wrong
  // channel when BaseApp has multiple into mgr.
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
}

void CellAppMgr::OnCreateSpaceRequest(const Address& src, Channel* ch,
                                      const cellappmgr::CreateSpaceRequest& msg) {
  if (msg.space_id == kInvalidSpaceID) {
    ATLAS_LOG_WARNING("CellAppMgr: CreateSpaceRequest with invalid space_id=0");
    SendCreateSpaceReply(msg, src, ch, /*ok=*/false, 0, Address{});
    return;
  }
  if (spaces_.contains(msg.space_id)) {
    ATLAS_LOG_WARNING("CellAppMgr: CreateSpaceRequest for existing space_id={}", msg.space_id);
    SendCreateSpaceReply(msg, src, ch, /*ok=*/false, 0, Address{});
    return;
  }
  const auto pending_it = std::find_if(
      pending_space_creates_awaiting_cellapps_.begin(),
      pending_space_creates_awaiting_cellapps_.end(),
      [space_id = msg.space_id](const PendingSpaceCreate& pending) {
        return pending.msg.space_id == space_id;
      });
  if (pending_it != pending_space_creates_awaiting_cellapps_.end()) {
    ATLAS_LOG_WARNING("CellAppMgr: CreateSpaceRequest for pending space_id={}", msg.space_id);
    SendCreateSpaceReply(msg, src, ch, /*ok=*/false, 0, Address{});
    return;
  }

  ATLAS_LOG_INFO(
      "CellAppMgr: queueing CreateSpaceRequest space_id={} (have {} cellapps, window {}ms)",
      msg.space_id, cellapps_.size(),
      std::chrono::duration_cast<std::chrono::milliseconds>(startup_quiescence_window_).count());
  const auto now = Clock::now();
  pending_space_creates_awaiting_cellapps_.push_back(
      {msg, src, ch, now, now + startup_quiescence_window_});
  // Zero-window path (tests) fires synchronously rather than next tick.
  DrainExpiredCreateSpaceRequests();
}

void CellAppMgr::ExecuteCreateSpace(const cellappmgr::CreateSpaceRequest& msg, const Address& src,
                                    Channel* ch) {
  if (spaces_.contains(msg.space_id)) {
    ATLAS_LOG_WARNING("CellAppMgr: deferred CreateSpace space_id={} already created — drop",
                      msg.space_id);
    SendCreateSpaceReply(msg, src, ch, /*ok=*/false, 0, Address{});
    return;
  }
  if (cellapps_.empty()) {
    ATLAS_LOG_WARNING("CellAppMgr: deferred CreateSpace space_id={} found 0 cellapps — fail",
                      msg.space_id);
    SendCreateSpaceReply(msg, src, ch, /*ok=*/false, 0, Address{});
    return;
  }

  // initial_cell_count is a ceiling clamped by SortedHostsForBootstrap.
  const std::size_t requested = std::max<std::size_t>(1, msg.initial_cell_count);
  auto hosts = SortedHostsForBootstrap(requested);
  if (hosts.empty()) {
    ATLAS_LOG_WARNING(
        "CellAppMgr: deferred CreateSpace space_id={} found 0 assignable cellapps",
        msg.space_id);
    SendCreateSpaceReply(msg, src, ch, /*ok=*/false, 0, Address{});
    return;
  }

  const cellappmgr::CellID first_cell_id = next_cell_id_++;
  CellInfo leaf;
  leaf.cell_id = first_cell_id;
  leaf.cellapp_addr = hosts[0]->internal_addr;
  leaf.load = hosts[0]->load;
  leaf.entity_count = 0;
  // Finite bounds; InitSingleCell adopts them so recursive Split midpoints
  // stay strictly inside the cell.
  leaf.bounds = CellBounds{-kDefaultWorldHalfExtent, -kDefaultWorldHalfExtent,
                           kDefaultWorldHalfExtent, kDefaultWorldHalfExtent};

  SpacePartition partition;
  partition.space_id = msg.space_id;
  partition.space_master_type = msg.space_master_type;
  partition.bsp.InitSingleCell(leaf);

  if (hosts.size() >= 2) BootstrapMultiCellPartition(partition, hosts);

  spaces_.emplace(msg.space_id, std::move(partition));
  auto& seeded = spaces_[msg.space_id];

  // Tell every host about its leaf and push the final geometry once.
  const auto primary_cell_id = seeded.bsp.PrimaryCellId();
  for (const auto* ci : seeded.bsp.Leaves()) {
    auto it = cellapps_.find(ci->cellapp_addr);
    if (it == cellapps_.end()) continue;
    const bool is_primary = ci->cell_id == primary_cell_id;
    SendAddCell(it->second, msg.space_id, ci->cell_id, ci->bounds, is_primary,
                seeded.space_master_type);
  }
  BroadcastGeometry(seeded);

  ATLAS_LOG_INFO("CellAppMgr: created Space {} with {} cell(s); primary host app_id={} ({}:{})",
                 msg.space_id, seeded.bsp.Leaves().size(), hosts[0]->app_id,
                 hosts[0]->internal_addr.Ip(), hosts[0]->internal_addr.Port());
  SendCreateSpaceReply(msg, src, ch, /*ok=*/true, first_cell_id, hosts[0]->internal_addr);
}

void CellAppMgr::DrainExpiredCreateSpaceRequests() {
  if (pending_space_creates_awaiting_cellapps_.empty()) return;
  const auto now = Clock::now();
  auto& q = pending_space_creates_awaiting_cellapps_;
  for (auto it = q.begin(); it != q.end();) {
    if (now >= it->quiescence_deadline && AssignableCellAppCount() > 0) {
      auto entry = std::move(*it);
      it = q.erase(it);
      ExecuteCreateSpace(entry.msg, entry.src, entry.ch);
    } else {
      ++it;
    }
  }
}

auto CellAppMgr::SortedHostsForBootstrap(std::size_t max) const -> std::vector<const CellAppInfo*> {
  std::vector<const CellAppInfo*> out;
  out.reserve(cellapps_.size());
  const auto now = Clock::now();
  for (const auto& [_, info] : cellapps_) {
    if (IsAssignableForLb(info, now)) out.push_back(&info);
  }
  std::sort(out.begin(), out.end(), [](const CellAppInfo* a, const CellAppInfo* b) {
    if (a->load != b->load) return a->load < b->load;
    return a->app_id < b->app_id;
  });
  if (out.size() > max) out.resize(max);
  return out;
}

void CellAppMgr::BootstrapMultiCellPartition(SpacePartition& partition,
                                             const std::vector<const CellAppInfo*>& hosts) {
  // Breadth-first split queue: alternate axis per tree level so an N=4
  // bootstrap lands as a 2x2 grid (matches tests/unit/test_bsp_tree).
  struct Pending {
    cellappmgr::CellID cell_id;
    int level;
  };
  std::queue<Pending> q;
  q.push({partition.bsp.Leaves().front()->cell_id, 0});

  std::size_t host_idx = 1;
  while (host_idx < hosts.size() && !q.empty()) {
    auto pend = q.front();
    q.pop();
    const BSPAxis axis = (pend.level % 2 == 0) ? BSPAxis::kX : BSPAxis::kZ;
    const auto* target = partition.bsp.FindCellById(pend.cell_id);
    if (target == nullptr) {
      ATLAS_LOG_ERROR("CellAppMgr: bootstrap split target missing cell_id={}", pend.cell_id);
      break;
    }
    const float position = MidpointForAxis(target->bounds, axis);
    CellInfo new_leaf;
    new_leaf.cell_id = next_cell_id_++;
    new_leaf.cellapp_addr = hosts[host_idx]->internal_addr;
    new_leaf.load = hosts[host_idx]->load;
    new_leaf.entity_count = 0;
    auto r = partition.bsp.Split(pend.cell_id, axis, position, new_leaf);
    if (!r) {
      ATLAS_LOG_ERROR("CellAppMgr: BSP split failed at level={} cell_id={}: {}", pend.level,
                      pend.cell_id, r.Error().Message());
      // Roll back the consumed cell_id so we don't leave a gap.
      --next_cell_id_;
      break;
    }
    ++host_idx;
    q.push({pend.cell_id, pend.level + 1});
    q.push({new_leaf.cell_id, pend.level + 1});
  }
}

auto CellAppMgr::PickHeaviestLeaf(const SpacePartition& partition) const -> const CellInfo* {
  const auto leaves = partition.bsp.Leaves();
  if (leaves.empty()) return nullptr;

  const CellInfo* target = leaves.front();
  for (const auto* leaf : leaves) {
    const bool heavier_load = leaf->load > target->load;
    const bool same_load = leaf->load == target->load;
    const bool more_entities = leaf->entity_count > target->entity_count;
    const bool same_entities = leaf->entity_count == target->entity_count;
    const bool lower_id = leaf->cell_id < target->cell_id;
    if (heavier_load || (same_load && more_entities) ||
        (same_load && same_entities && lower_id)) {
      target = leaf;
    }
  }
  return target;
}

auto CellAppMgr::SplitLeafToHost(SpacePartition& partition, const CellInfo& target,
                                 const CellAppInfo& new_app, const char* reason) -> bool {
  if (!IsAssignableForLb(new_app, Clock::now())) return false;
  const auto space_id = partition.space_id;
  const auto target_cell_id = target.cell_id;
  const auto target_bounds = target.bounds;
  const auto source_app_id = AppIdForAddress(target.cellapp_addr);
  const auto before_version = partition.geometry_version;
  const auto before_topology = SnapshotLeafTopology(partition);
  const float dx = target_bounds.max_x - target_bounds.min_x;
  const float dz = target_bounds.max_z - target_bounds.min_z;
  const bool dx_finite = std::isfinite(dx);
  const bool dz_finite = std::isfinite(dz);
  BSPAxis axis = BSPAxis::kX;
  if (dx_finite && dz_finite) {
    axis = (dx >= dz) ? BSPAxis::kX : BSPAxis::kZ;
  } else if (dz_finite) {
    axis = BSPAxis::kZ;
  }

  auto dist_it = cell_distributions_.find(target_cell_id);
  const bool has_dist = dist_it != cell_distributions_.end() && dist_it->second.entity_count > 0;
  float position;
  if (has_dist) {
    const auto& count_buckets =
        axis == BSPAxis::kX ? dist_it->second.x_buckets : dist_it->second.z_buckets;
    const auto& load_buckets =
        axis == BSPAxis::kX ? dist_it->second.x_load_buckets : dist_it->second.z_load_buckets;
    const auto buckets = BucketWeights(count_buckets, load_buckets);
    if (auto bucket_pos = BucketSplitPosition(buckets, target_bounds, axis)) {
      position = *bucket_pos;
    } else {
      position = (axis == BSPAxis::kX) ? dist_it->second.median_x : dist_it->second.median_z;
    }
  } else if (dx_finite && dz_finite) {
    position = (axis == BSPAxis::kX) ? (target_bounds.min_x + target_bounds.max_x) * 0.5f
                                     : (target_bounds.min_z + target_bounds.max_z) * 0.5f;
  } else if (axis == BSPAxis::kX && dx_finite) {
    position = (target_bounds.min_x + target_bounds.max_x) * 0.5f;
  } else if (axis == BSPAxis::kZ && dz_finite) {
    position = (target_bounds.min_z + target_bounds.max_z) * 0.5f;
  } else {
    position = 0.f;
  }
  position = ClampInsideAxis(position, target_bounds, axis);

  CellInfo new_leaf;
  new_leaf.cell_id = next_cell_id_++;
  new_leaf.cellapp_addr = new_app.internal_addr;
  new_leaf.load = 0.f;
  new_leaf.entity_count = 0;

  auto r = partition.bsp.Split(target_cell_id, axis, position, new_leaf);
  if (!r) {
    ATLAS_LOG_WARNING("CellAppMgr: {} Split failed space={} cell={}: {}", reason, space_id,
                      target_cell_id, r.Error().Message());
    --next_cell_id_;
    return false;
  }

  const auto new_cell_id = new_leaf.cell_id;
  const auto* new_leaf_in_tree = partition.bsp.FindCellById(new_cell_id);
  if (new_leaf_in_tree != nullptr) {
    SendAddCell(new_app, space_id, new_cell_id, new_leaf_in_tree->bounds,
                /*is_primary=*/false, /*space_master_type=*/"");
  }
  pending_geometry_broadcasts_.push_back({space_id, new_cell_id, new_app.internal_addr,
                                          Clock::now()});
  hot_leaf_balance_ticks_.erase(target_cell_id);
  hot_leaf_balance_ticks_.erase(new_cell_id);

  auto detail = std::format("axis={},pos={:.3f},deferred=1,dist={}",
                            axis == BSPAxis::kX ? "x" : "z", position, has_dist ? 1 : 0);
  RecordLbDecision("split", reason, space_id, target_cell_id, new_cell_id, source_app_id,
                   new_app.app_id, partition.geometry_version,
                   AppendTopologyDiff(std::move(detail), partition, before_version,
                                      before_topology));
  ATLAS_LOG_INFO(
      "CellAppMgr: {} space={} split cell={} on axis={} pos={} -> new cell={} "
      "on app_id={} (geometry deferred until ack)",
      reason, space_id, target_cell_id, static_cast<int>(axis), position, new_cell_id,
      new_app.app_id);
  return true;
}

void CellAppMgr::GrowSpacesForNewCellApp(const CellAppInfo& new_app) {
  if (RecoveryWindowActive()) return;
  if (!IsAssignableForLb(new_app, Clock::now())) return;
  const auto assignable_count = AssignableCellAppCount();
  for (auto& [_, partition] : spaces_) {
    if (partition.bsp.Leaves().size() >= assignable_count) continue;
    const auto* target = PickHeaviestLeaf(partition);
    if (target == nullptr) continue;
    (void)SplitLeafToHost(partition, *target, new_app, "elastic-grow");
  }
}

auto CellAppMgr::PickIdleHostForAutoSplit(const SpacePartition& partition) const
    -> const CellAppInfo* {
  const auto leaves = partition.bsp.Leaves();
  if (leaves.size() >= AssignableCellAppCount()) return nullptr;
  const CellAppInfo* best = nullptr;
  const float idle_threshold = NonNegative(s_lb_auto_split_idle_load_threshold.Value());
  const auto now = Clock::now();
  for (const auto& [addr, info] : cellapps_) {
    if (!IsAssignableForLb(info, now)) continue;
    if (info.load > idle_threshold) continue;
    bool already_hosts_space = false;
    for (const auto* leaf : leaves) {
      if (leaf->cellapp_addr == addr) {
        already_hosts_space = true;
        break;
      }
    }
    if (already_hosts_space) continue;
    if (best == nullptr || info.load < best->load ||
        (info.load == best->load && info.app_id < best->app_id)) {
      best = &info;
    }
  }
  return best;
}

auto CellAppMgr::PickRetireDrainTarget(const SpacePartition& partition,
                                       const CellInfo& source_leaf) const
    -> const CellAppInfo* {
  auto is_better = [](const CellAppInfo* best, const CellAppInfo& candidate) {
    if (best == nullptr) return true;
    if (candidate.load != best->load) return candidate.load < best->load;
    return candidate.app_id < best->app_id;
  };

  const CellAppInfo* in_space = nullptr;
  const auto now = Clock::now();
  for (const auto* leaf : partition.bsp.Leaves()) {
    if (leaf->cellapp_addr == source_leaf.cellapp_addr) continue;
    auto it = cellapps_.find(leaf->cellapp_addr);
    if (it == cellapps_.end() || !IsAssignableForLb(it->second, now) ||
        it->second.channel == nullptr) {
      continue;
    }
    if (is_better(in_space, it->second)) in_space = &it->second;
  }
  if (in_space != nullptr) return in_space;

  const CellAppInfo* any = nullptr;
  for (const auto& [addr, info] : cellapps_) {
    if (addr == source_leaf.cellapp_addr || !IsAssignableForLb(info, now) ||
        info.channel == nullptr) {
      continue;
    }
    if (is_better(any, info)) any = &info;
  }
  return any;
}

auto CellAppMgr::TryAutoSplitHotLeaf(SpacePartition& partition) -> bool {
  const auto* host = PickIdleHostForAutoSplit(partition);
  if (host == nullptr) return false;

  const float load_threshold = NonNegative(s_lb_auto_split_load_threshold.Value());
  const uint32_t sustain_ticks = std::max<uint32_t>(1, s_lb_auto_split_sustain_ticks.Value());
  const auto now = Clock::now();
  const CellInfo* target = nullptr;
  for (const auto* leaf : partition.bsp.Leaves()) {
    const auto app_it = cellapps_.find(leaf->cellapp_addr);
    if (app_it == cellapps_.end() || !IsAssignableForLb(app_it->second, now) ||
        !std::isfinite(leaf->load) || leaf->load < load_threshold ||
        leaf->entity_count == 0) {
      hot_leaf_balance_ticks_.erase(leaf->cell_id);
      continue;
    }
    const uint32_t ticks = ++hot_leaf_balance_ticks_[leaf->cell_id];
    if (ticks < sustain_ticks) continue;
    if (target == nullptr || leaf->load > target->load ||
        (leaf->load == target->load && leaf->entity_count > target->entity_count) ||
        (leaf->load == target->load && leaf->entity_count == target->entity_count &&
         leaf->cell_id < target->cell_id)) {
      target = leaf;
    }
  }
  if (target == nullptr) return false;
  return SplitLeafToHost(partition, *target, *host, "auto-split");
}

auto CellAppMgr::PickMergeCandidate(const SpacePartition& partition)
    -> std::optional<MergeCandidate> {
  const float threshold = NonNegative(s_lb_auto_merge_load_threshold.Value());
  const uint32_t sustain_ticks = std::max<uint32_t>(1, s_lb_auto_merge_sustain_ticks.Value());
  const auto primary = partition.bsp.PrimaryCellId();
  const auto now = Clock::now();
  auto has_fresh_assignable_owner = [&](const CellInfo& leaf) {
    const auto app_it = cellapps_.find(leaf.cellapp_addr);
    return app_it != cellapps_.end() && IsAssignableForLb(app_it->second, now);
  };

  for (const auto* leaf : partition.bsp.Leaves()) {
    const bool eligible = has_fresh_assignable_owner(*leaf) &&
                          leaf->cell_id != primary && leaf->entity_count == 0 &&
                          std::isfinite(leaf->load) && leaf->load <= threshold;
    if (!eligible) idle_leaf_balance_ticks_.erase(leaf->cell_id);
  }

  std::optional<MergeCandidate> best;
  float best_combined_load = std::numeric_limits<float>::infinity();
  for (const auto& [left_id, right_id] : partition.bsp.LeafSiblingPairs()) {
    const auto* left = partition.bsp.FindCellById(left_id);
    const auto* right = partition.bsp.FindCellById(right_id);
    if (left == nullptr || right == nullptr) continue;
    if (!has_fresh_assignable_owner(*left) || !has_fresh_assignable_owner(*right) ||
        !std::isfinite(left->load) || !std::isfinite(right->load) ||
        left->load > threshold || right->load > threshold) {
      idle_leaf_balance_ticks_.erase(left_id);
      idle_leaf_balance_ticks_.erase(right_id);
      continue;
    }

    const CellInfo* remove = nullptr;
    const CellInfo* keep = nullptr;
    const bool left_removable = left->cell_id != primary && left->entity_count == 0;
    const bool right_removable = right->cell_id != primary && right->entity_count == 0;
    if (left_removable && right_removable) {
      remove = left->cell_id > right->cell_id ? left : right;
      keep = remove == left ? right : left;
    } else if (left_removable) {
      remove = left;
      keep = right;
    } else if (right_removable) {
      remove = right;
      keep = left;
    } else {
      continue;
    }

    const uint32_t ticks = ++idle_leaf_balance_ticks_[remove->cell_id];
    if (ticks < sustain_ticks) continue;
    const float combined_load = left->load + right->load;
    if (!best || combined_load < best_combined_load ||
        (combined_load == best_combined_load && remove->cell_id < best->remove_cell_id)) {
      best_combined_load = combined_load;
      best = MergeCandidate{remove->cell_id, keep->cell_id, remove->cellapp_addr};
    }
  }
  return best;
}

auto CellAppMgr::TryAutoMergeIdleLeaf(SpacePartition& partition) -> bool {
  auto candidate = PickMergeCandidate(partition);
  if (!candidate) return false;

  auto host_it = cellapps_.find(candidate->remove_addr);
  const auto before_version = partition.geometry_version;
  const auto before_topology = SnapshotLeafTopology(partition);
  auto r = partition.bsp.Unsplit(candidate->remove_cell_id);
  if (!r) {
    ATLAS_LOG_WARNING("CellAppMgr: auto-merge Unsplit failed space={} cell={}: {}",
                      partition.space_id, candidate->remove_cell_id, r.Error().Message());
    idle_leaf_balance_ticks_.erase(candidate->remove_cell_id);
    return false;
  }

  cell_distributions_.erase(candidate->remove_cell_id);
  hot_leaf_balance_ticks_.erase(candidate->remove_cell_id);
  idle_leaf_balance_ticks_.erase(candidate->remove_cell_id);
  idle_leaf_balance_ticks_.erase(candidate->keep_cell_id);
  BroadcastGeometry(partition);
  if (host_it != cellapps_.end()) {
    SendRemoveCell(host_it->second, partition.space_id, candidate->remove_cell_id);
  }

  const auto* kept = partition.bsp.FindCellById(candidate->keep_cell_id);
  RecordLbDecision("merge", "auto-merge", partition.space_id, candidate->remove_cell_id,
                   candidate->keep_cell_id, AppIdForAddress(candidate->remove_addr),
                   kept == nullptr ? 0 : AppIdForAddress(kept->cellapp_addr),
                   partition.geometry_version,
                   AppendTopologyDiff("removed_empty=1", partition, before_version,
                                      before_topology));
  ATLAS_LOG_INFO("CellAppMgr: auto-merge space={} removed cell={} into sibling cell={}",
                 partition.space_id, candidate->remove_cell_id, candidate->keep_cell_id);
  return true;
}

auto CellAppMgr::TryRetireOneLeaf(SpacePartition& partition) -> bool {
  const auto primary = partition.bsp.PrimaryCellId();
  const CellInfo* target = nullptr;
  const CellAppInfo* target_app = nullptr;
  const auto now = Clock::now();
  for (const auto* leaf : partition.bsp.Leaves()) {
    if (leaf->cell_id == primary || leaf->entity_count != 0) continue;
    auto app_it = cellapps_.find(leaf->cellapp_addr);
    if (app_it == cellapps_.end() || !app_it->second.is_retiring) continue;
    if (!HasFreshLoadReport(app_it->second, now)) continue;
    if (target == nullptr || app_it->second.app_id < target_app->app_id ||
        (app_it->second.app_id == target_app->app_id && leaf->cell_id < target->cell_id)) {
      target = leaf;
      target_app = &app_it->second;
    }
  }
  if (target == nullptr || target_app == nullptr) return false;

  const auto remove_cell_id = target->cell_id;
  const auto remove_addr = target->cellapp_addr;
  const auto app_id = target_app->app_id;
  const auto before_version = partition.geometry_version;
  const auto before_topology = SnapshotLeafTopology(partition);
  auto r = partition.bsp.Unsplit(remove_cell_id);
  if (!r) {
    ATLAS_LOG_WARNING("CellAppMgr: retire Unsplit failed space={} cell={}: {}",
                      partition.space_id, remove_cell_id, r.Error().Message());
    return false;
  }

  cell_distributions_.erase(remove_cell_id);
  hot_leaf_balance_ticks_.erase(remove_cell_id);
  idle_leaf_balance_ticks_.erase(remove_cell_id);
  BroadcastGeometry(partition);
  if (auto host_it = cellapps_.find(remove_addr); host_it != cellapps_.end()) {
    SendRemoveCell(host_it->second, partition.space_id, remove_cell_id);
  }

  RecordLbDecision("remove", "retire-empty", partition.space_id, remove_cell_id, 0, app_id, 0,
                   partition.geometry_version,
                   AppendTopologyDiff("empty=1", partition, before_version, before_topology));
  ATLAS_LOG_INFO("CellAppMgr: retire removed empty cell={} from app_id={} space={}",
                 remove_cell_id, app_id, partition.space_id);
  return true;
}

auto CellAppMgr::TryRetireHandoffLeaf(SpacePartition& partition) -> bool {
  const auto primary = partition.bsp.PrimaryCellId();
  const CellInfo* source = nullptr;
  const CellAppInfo* source_app = nullptr;
  const CellAppInfo* target = nullptr;
  const auto now = Clock::now();
  for (const auto* leaf : partition.bsp.Leaves()) {
    const bool is_primary = leaf->cell_id == primary;
    if (!is_primary && leaf->entity_count == 0) continue;
    const auto active = std::any_of(retire_drains_.begin(), retire_drains_.end(),
                                    [&](const RetireDrain& drain) {
                                      return drain.space_id == partition.space_id &&
                                             drain.cell_id == leaf->cell_id &&
                                             drain.source_addr == leaf->cellapp_addr;
                                    });
    if (active) continue;
    auto app_it = cellapps_.find(leaf->cellapp_addr);
    if (app_it == cellapps_.end() || !app_it->second.is_retiring) continue;
    if (!HasFreshLoadReport(app_it->second, now)) continue;
    const auto* leaf_target = PickRetireDrainTarget(partition, *leaf);
    if (leaf_target == nullptr) continue;
    if (source == nullptr || app_it->second.app_id < source_app->app_id ||
        (app_it->second.app_id == source_app->app_id && leaf->cell_id < source->cell_id)) {
      source = leaf;
      source_app = &app_it->second;
      target = leaf_target;
    }
  }
  if (source == nullptr || source_app == nullptr || target == nullptr) return false;

  const auto source_addr = source->cellapp_addr;
  const auto cell_id = source->cell_id;
  const auto bounds = source->bounds;
  const bool is_primary = cell_id == primary;
  const auto before_version = partition.geometry_version;
  const auto before_topology = SnapshotLeafTopology(partition);
  auto* mutable_leaf = partition.bsp.FindCellByIdMutable(cell_id);
  if (mutable_leaf == nullptr) return false;

  const Address snapshot_source = is_primary ? source_addr : Address{};
  SendAddCell(*target, partition.space_id, cell_id, bounds, is_primary, /*space_master_type=*/"",
              snapshot_source);

  mutable_leaf->cellapp_addr = target->internal_addr;
  mutable_leaf->load = target->load;

  pending_geometry_broadcasts_.push_back(
      {partition.space_id, cell_id, target->internal_addr, Clock::now(),
       {ExtraGeometryRecipient{source_addr, cell_id}}, !is_primary});
  RetireDrain drain;
  drain.space_id = partition.space_id;
  drain.cell_id = cell_id;
  drain.source_addr = source_addr;
  drain.target_addr = target->internal_addr;
  drain.last_entity_count = source->entity_count;
  drain.started_at = Clock::now();
  drain.last_progress_at = drain.started_at;
  retire_drains_.push_back(drain);
  hot_leaf_balance_ticks_.erase(cell_id);
  idle_leaf_balance_ticks_.erase(cell_id);

  RecordLbDecision("handoff", is_primary ? "retire-primary" : "retire-drain",
                   partition.space_id, cell_id, cell_id, source_app->app_id,
                   target->app_id, partition.geometry_version,
                   AppendTopologyDiff(std::format("primary={},deferred=1",
                                                  is_primary ? 1 : 0),
                                      partition, before_version, before_topology));
  ATLAS_LOG_INFO(
      "CellAppMgr: retire handoff space={} cell={} from app_id={} to app_id={} "
      "primary={} (geometry deferred until ack)",
      partition.space_id, cell_id, source_app->app_id, target->app_id, is_primary ? 1 : 0);
  return true;
}

void CellAppMgr::OnAddCellToSpaceAck(const Address& src, Channel* ch,
                                     const cellappmgr::AddCellToSpaceAck& msg) {
  auto it = std::find_if(pending_geometry_broadcasts_.begin(), pending_geometry_broadcasts_.end(),
                         [&](const PendingGeometryBroadcast& p) {
                           return p.space_id == msg.space_id && p.awaiting_cell_id == msg.cell_id;
                         });
  if (it == pending_geometry_broadcasts_.end()) {
    // Stray ack - bootstrap path doesn't defer, and a timed-out elastic-grow
    // already broadcast. Logging this is too noisy in steady state.
    return;
  }
  const auto& pending_ref = *it;
  const auto app_it = cellapps_.find(pending_ref.awaiting_addr);
  if (app_it == cellapps_.end()) {
    ATLAS_LOG_WARNING(
        "CellAppMgr: ignoring AddCellToSpaceAck for missing target space={} cell={} addr={}",
        msg.space_id, msg.cell_id, pending_ref.awaiting_addr.ToString());
    return;
  }
  const bool channel_matches = app_it->second.channel != nullptr && app_it->second.channel == ch;
  if (src != pending_ref.awaiting_addr && !channel_matches) {
    ATLAS_LOG_WARNING(
        "CellAppMgr: ignoring AddCellToSpaceAck from {} for space={} cell={} expected={}",
        src.ToString(), msg.space_id, msg.cell_id, pending_ref.awaiting_addr.ToString());
    return;
  }
  if (!msg.success) {
    ATLAS_LOG_WARNING("CellAppMgr: AddCellToSpaceAck failed from {} for space={} cell={}",
                      pending_ref.awaiting_addr.ToString(), msg.space_id, msg.cell_id);
    return;
  }
  auto pending = std::move(*it);
  const SpaceID space_id = pending.space_id;
  pending_geometry_broadcasts_.erase(it);

  auto sp_it = spaces_.find(space_id);
  if (sp_it == spaces_.end()) return;
  BroadcastGeometry(sp_it->second, pending.extra_recipients);
  MarkRetireDrainGeometryPublished(space_id, pending.awaiting_cell_id, pending.awaiting_addr);
}

void CellAppMgr::DrainPendingGeometryBroadcasts() {
  if (pending_geometry_broadcasts_.empty()) return;
  const auto now = Clock::now();
  for (auto it = pending_geometry_broadcasts_.begin(); it != pending_geometry_broadcasts_.end();) {
    if (now - it->sent_at < kPendingGeometryTimeout) {
      ++it;
      continue;
    }
    if (!it->allow_timeout_broadcast) {
      ATLAS_LOG_WARNING(
          "CellAppMgr: AddCellToSpaceAck timeout space={} cell={} addr={}:{} - "
          "holding geometry until primary handoff acks",
          it->space_id, it->awaiting_cell_id, it->awaiting_addr.Ip(), it->awaiting_addr.Port());
      it->sent_at = now;
      ++it;
      continue;
    }
    ATLAS_LOG_WARNING(
        "CellAppMgr: AddCellToSpaceAck timeout space={} cell={} addr={}:{} - broadcasting "
        "geometry anyway; receiver may have a brief offload-into-missing-cell window",
        it->space_id, it->awaiting_cell_id, it->awaiting_addr.Ip(), it->awaiting_addr.Port());
    const auto space_id = it->space_id;
    const auto cell_id = it->awaiting_cell_id;
    const auto target_addr = it->awaiting_addr;
    auto sp_it = spaces_.find(it->space_id);
    if (sp_it != spaces_.end()) BroadcastGeometry(sp_it->second, it->extra_recipients);
    MarkRetireDrainGeometryPublished(space_id, cell_id, target_addr);
    it = pending_geometry_broadcasts_.erase(it);
  }
}

void CellAppMgr::MarkRetireDrainGeometryPublished(SpaceID space_id,
                                                  cellappmgr::CellID cell_id,
                                                  const Address& target_addr) {
  const auto now = Clock::now();
  for (auto& drain : retire_drains_) {
    if (drain.space_id != space_id || drain.cell_id != cell_id ||
        drain.target_addr != target_addr) {
      continue;
    }
    drain.geometry_published = true;
    drain.started_at = now;
    drain.last_progress_at = now;
    drain.last_watchdog_log_at = {};
  }
}

auto CellAppMgr::IsRetireDrainStuck(const RetireDrain& drain, TimePoint now) const -> bool {
  if (!drain.geometry_published || drain.last_entity_count == 0) return false;
  return now - drain.last_progress_at >= RetireDrainWatchdogWindow();
}

void CellAppMgr::AuditRetireDrainWatchdog() {
  if (retire_drains_.empty()) return;
  const auto now = Clock::now();
  const auto watchdog_window = RetireDrainWatchdogWindow();
  const auto min_repeat_window = std::chrono::duration_cast<Duration>(Milliseconds{1000});
  const auto repeat_window =
      watchdog_window < min_repeat_window ? min_repeat_window : watchdog_window;
  for (auto& drain : retire_drains_) {
    if (!IsRetireDrainStuck(drain, now)) continue;
    if (drain.last_watchdog_log_at != TimePoint{} &&
        now - drain.last_watchdog_log_at < repeat_window) {
      continue;
    }
    ATLAS_LOG_WARNING(
        "CellAppMgr: retire drain stuck space={} cell={} source={} target={} "
        "entities={} idle_ms={} drain_ms={}",
        drain.space_id, drain.cell_id, drain.source_addr.ToString(),
        drain.target_addr.ToString(), drain.last_entity_count,
        DurationMs(now - drain.last_progress_at), DurationMs(now - drain.started_at));
    drain.last_watchdog_log_at = now;
  }
}

void CellAppMgr::OnCellAppDeath(const Address& internal_addr, uint8_t reason) {
  auto it = cellapps_.find(internal_addr);
  if (it == cellapps_.end()) return;
  const uint32_t dead_app_id = it->second.app_id;
  cellapps_.erase(it);
  retire_drains_.erase(std::remove_if(retire_drains_.begin(), retire_drains_.end(),
                                      [&](const RetireDrain& drain) {
                                        return drain.source_addr == internal_addr ||
                                               drain.target_addr == internal_addr;
                                      }),
                       retire_drains_.end());
  for (auto pending_it = pending_geometry_broadcasts_.begin();
       pending_it != pending_geometry_broadcasts_.end();) {
    if (pending_it->awaiting_addr == internal_addr) {
      pending_it = pending_geometry_broadcasts_.erase(pending_it);
      continue;
    }
    pending_it->extra_recipients.erase(
        std::remove_if(pending_it->extra_recipients.begin(),
                       pending_it->extra_recipients.end(),
                       [&](const ExtraGeometryRecipient& extra) {
                         return extra.addr == internal_addr;
                       }),
    pending_it->extra_recipients.end());
    ++pending_it;
  }

  // BaseApp restores Reals from backup; mgr only re-points BSP leaves
  // so future CreateCellEntity / Offload traffic reaches a survivor.
  if (cellapps_.empty()) {
    if (reason == 0) {
      ATLAS_LOG_INFO(
          "CellAppMgr: CellApp app_id={} deregistered and no survivors remain",
          dead_app_id);
    } else {
      ATLAS_LOG_CRITICAL(
          "CellAppMgr: CellApp app_id={} died and no survivors remain — all "
          "BSP leaves orphaned until a new CellApp registers",
          dead_app_id);
    }
    return;
  }

  // Per-space fallback hosts plus detailed leaf bounds for BaseApp
  // position-based restore target selection.
  std::vector<std::pair<SpaceID, Address>> rehomes;
  std::vector<baseapp::CellAppDeath::RehomeCell> rehome_cells;

  for (auto& [space_id, partition] : spaces_) {
    // Snapshot first because Unsplit mutates the tree mid-iteration.
    std::vector<cellappmgr::CellID> orphan_ids;
    for (const auto* leaf : partition.bsp.Leaves()) {
      if (leaf->cellapp_addr == internal_addr) orphan_ids.push_back(leaf->cell_id);
    }
    if (orphan_ids.empty()) continue;

    bool topology_changed = false;
    Address first_new_host{};
    for (auto cid : orphan_ids) {
      const auto* dead_leaf = partition.bsp.FindCellById(cid);
      const CellBounds dead_bounds = dead_leaf != nullptr ? dead_leaf->bounds : CellBounds{};
      const auto before_version = partition.geometry_version;
      const auto before_topology = SnapshotLeafTopology(partition, internal_addr, dead_app_id);
      cell_distributions_.erase(cid);
      hot_leaf_balance_ticks_.erase(cid);
      idle_leaf_balance_ticks_.erase(cid);

      auto r = partition.bsp.Unsplit(cid);
      if (r.HasValue()) {
        const auto [mid_x, mid_z] = BoundsMidpoint(dead_bounds);
        if (const auto* absorbing = partition.bsp.FindCell(mid_x, mid_z)) {
          if (first_new_host.Ip() == 0) first_new_host = absorbing->cellapp_addr;
          RecordLbDecision("unsplit", "cellapp-death", space_id, cid, absorbing->cell_id,
                           dead_app_id, AppIdForAddress(absorbing->cellapp_addr),
                           partition.geometry_version,
                           AppendTopologyDiff("sibling_absorb=1,broadcast_pending=1", partition,
                                              before_version, before_topology));
        }
        ATLAS_LOG_INFO("CellAppMgr: unsplit cell_id={} (space {}) — sibling subtree absorbs bounds",
                       cid, space_id);
        topology_changed = true;
        continue;
      }
      const auto* alt = PickAlternateHostInSpace(internal_addr, partition);
      if (alt == nullptr) alt = PickAlternateHost(internal_addr);
      if (alt == nullptr) {
        ATLAS_LOG_ERROR("CellAppMgr: rehoming cell_id={} (space {}) failed — no alternate host",
                        cid, space_id);
        break;
      }
      auto* leaf = partition.bsp.FindCellByIdMutable(cid);
      if (leaf == nullptr) continue;
      ATLAS_LOG_INFO(
          "CellAppMgr: rehoming cell_id={} (space {}) from dead app_id={} to survivor app_id={}",
          cid, space_id, dead_app_id, alt->app_id);
      leaf->cellapp_addr = alt->internal_addr;
      leaf->load = alt->load;
      SendAddCell(*alt, space_id, leaf->cell_id, leaf->bounds, /*is_primary=*/false,
                  partition.space_master_type);
      RecordLbDecision("rehome", "cellapp-death", space_id, cid, cid, dead_app_id,
                       alt->app_id, partition.geometry_version,
                       AppendTopologyDiff("add_cell=1,broadcast_pending=1", partition,
                                          before_version, before_topology));
      topology_changed = true;
      if (first_new_host.Ip() == 0) first_new_host = alt->internal_addr;
    }

    if (topology_changed) {
      BroadcastGeometry(partition);
      if (first_new_host.Ip() == 0) {
        if (const auto* primary = partition.bsp.FindCellById(partition.bsp.PrimaryCellId())) {
          first_new_host = primary->cellapp_addr;
        }
      }
      rehomes.emplace_back(space_id, first_new_host);
      for (const auto* leaf : partition.bsp.Leaves()) {
        rehome_cells.push_back(baseapp::CellAppDeath::RehomeCell{
            space_id, leaf->cell_id, leaf->cellapp_addr, leaf->bounds});
      }
    }
  }

  // Direct path to every BaseApp; machined subscription owns the map.
  if (!baseapps_.empty()) {
    baseapp::CellAppDeath notify;
    notify.dead_addr = internal_addr;
    notify.rehomes = std::move(rehomes);
    notify.rehome_cells = std::move(rehome_cells);
    for (const auto& [addr, ch] : baseapps_) {
      if (ch != nullptr) (void)ch->SendMessage(notify);
    }
  }
}

auto CellAppMgr::HasPendingGeometryBroadcast(SpaceID space_id) const -> bool {
  return std::any_of(pending_geometry_broadcasts_.begin(), pending_geometry_broadcasts_.end(),
                     [space_id](const PendingGeometryBroadcast& p) {
                       return p.space_id == space_id;
                     });
}

void CellAppMgr::TickLoadBalance() {
  if (spaces_.empty()) return;
  if (RecoveryWindowActive()) return;
  for (auto& [space_id, partition] : spaces_) {
    if (HasPendingGeometryBroadcast(space_id)) continue;
    const auto before_blob = partition.last_broadcast_blob;
    const auto before_version = partition.geometry_version;
    const auto before_topology = SnapshotLeafTopology(partition);
    partition.bsp.Balance(kBalanceSafetyBound);
    if (TryRetireOneLeaf(partition)) continue;
    if (TryRetireHandoffLeaf(partition)) continue;
    if (TryAutoSplitHotLeaf(partition)) continue;
    if (TryAutoMergeIdleLeaf(partition)) continue;
    BroadcastGeometry(partition);
    if (partition.last_broadcast_blob != before_blob) {
      RecordLbDecision("balance", "weighted-load", space_id, 0, 0, 0, 0,
                       partition.geometry_version,
                       AppendTopologyDiff("leaves_balanced=1", partition, before_version,
                                          before_topology));
      ATLAS_LOG_INFO("CellAppMgr: LB balance updated {}", BuildSpaceLoadSummary(partition));
    }
  }
}

auto CellAppMgr::BuildCellAppLoadSummary() const -> std::string {
  if (cellapps_.empty()) return "cellapps=0";
  std::vector<const CellAppInfo*> apps;
  apps.reserve(cellapps_.size());
  for (const auto& [_, info] : cellapps_) apps.push_back(&info);
  std::sort(apps.begin(), apps.end(), [](const CellAppInfo* a, const CellAppInfo* b) {
    return a->app_id < b->app_id;
  });

  const auto now = Clock::now();
  std::string out = std::format("cellapps={}", apps.size());
  for (const auto* app : apps) {
    const auto load_age = now - LastLoadReportAt(*app);
    const bool load_stale = IsLoadReportStale(*app, now);
    out += std::format(" app={} addr={} load={:.3f} entities={} retiring={} "
                       "load_age_ms={} load_stale={}",
                       app->app_id, app->internal_addr.ToString(), app->load,
                       app->entity_count, app->is_retiring ? 1 : 0,
                       DurationMs(load_age), load_stale ? 1 : 0);
  }
  return out;
}

auto CellAppMgr::BuildPendingSpaceCreateSummary() const -> std::string {
  if (pending_space_creates_awaiting_cellapps_.empty()) return "pending=0";
  const auto now = Clock::now();
  std::string out =
      std::format("pending={} assignable={}",
                  pending_space_creates_awaiting_cellapps_.size(), AssignableCellAppCount());
  for (const auto& pending : pending_space_creates_awaiting_cellapps_) {
    const int64_t age_ms = std::max<int64_t>(0, DurationMs(now - pending.queued_at));
    const int64_t deadline_ms =
        std::max<int64_t>(0, DurationMs(pending.quiescence_deadline - now));
    out += std::format(" space={} request={} age_ms={} deadline_in_ms={}",
                       pending.msg.space_id, pending.msg.request_id, age_ms, deadline_ms);
  }
  return out;
}

auto CellAppMgr::BuildSpaceLoadSummary() const -> std::string {
  if (spaces_.empty()) return "spaces=0";
  std::vector<const SpacePartition*> partitions;
  partitions.reserve(spaces_.size());
  for (const auto& [_, partition] : spaces_) partitions.push_back(&partition);
  std::sort(partitions.begin(), partitions.end(),
            [](const SpacePartition* a, const SpacePartition* b) {
              return a->space_id < b->space_id;
            });

  std::string out = std::format("spaces={}", partitions.size());
  for (const auto* partition : partitions) {
    out += " | ";
    out += BuildSpaceLoadSummary(*partition);
  }
  return out;
}

auto CellAppMgr::BuildSpaceLoadSummary(const SpacePartition& partition) const -> std::string {
  auto leaves = partition.bsp.Leaves();
  std::sort(leaves.begin(), leaves.end(), [](const CellInfo* a, const CellInfo* b) {
    return a->cell_id < b->cell_id;
  });

  const auto pending = std::count_if(
      pending_geometry_broadcasts_.begin(), pending_geometry_broadcasts_.end(),
      [space_id = partition.space_id](const PendingGeometryBroadcast& p) {
        return p.space_id == space_id;
      });
  std::string out = std::format("space={} version={} freeze_epoch={} leaves={} primary={} "
                                "pending_ack={}",
                                partition.space_id, partition.geometry_version,
                                partition.freeze_epoch, leaves.size(),
                                partition.bsp.PrimaryCellId(), pending);
  for (const auto* leaf : leaves) {
    uint32_t app_id = 0;
    if (auto app_it = cellapps_.find(leaf->cellapp_addr); app_it != cellapps_.end()) {
      app_id = app_it->second.app_id;
    }
    out += std::format(
        " cell={} app={} load={:.3f} tick={:.3f} script_us={} native_us={} entities={} "
        "witnesses={} "
        "aoi_peers={} "
        "aoi_bytes={}/{} backup_bytes={} bounds=({:.1f},{:.1f},{:.1f},{:.1f})",
        leaf->cell_id, app_id, leaf->load, leaf->tick_load, leaf->script_tick_us,
        leaf->native_tick_us, leaf->entity_count, leaf->witness_count, leaf->aoi_peer_count,
        leaf->aoi_reliable_bytes, leaf->aoi_unreliable_bytes, leaf->backup_bytes,
        leaf->bounds.min_x, leaf->bounds.min_z, leaf->bounds.max_x, leaf->bounds.max_z);
    if (auto hot_it = hot_leaf_balance_ticks_.find(leaf->cell_id);
        hot_it != hot_leaf_balance_ticks_.end()) {
      out += std::format(" hot_ticks={}", hot_it->second);
    }
    if (auto idle_it = idle_leaf_balance_ticks_.find(leaf->cell_id);
        idle_it != idle_leaf_balance_ticks_.end()) {
      out += std::format(" idle_ticks={}", idle_it->second);
    }

    auto dist_it = cell_distributions_.find(leaf->cell_id);
    if (dist_it == cell_distributions_.end() || dist_it->second.entity_count == 0) {
      out += " median=na";
      continue;
    }
    out += std::format(" median=({:.1f},{:.1f})", dist_it->second.median_x,
                       dist_it->second.median_z);
    out += std::format(" xb={} zb={}", FormatBuckets(dist_it->second.x_buckets),
                       FormatBuckets(dist_it->second.z_buckets));
    if (BucketTotal(dist_it->second.x_load_buckets) > 0 ||
        BucketTotal(dist_it->second.z_load_buckets) > 0) {
      out += std::format(" xlb={} zlb={}", FormatBuckets(dist_it->second.x_load_buckets),
                         FormatBuckets(dist_it->second.z_load_buckets));
    }
  }
  return out;
}

auto CellAppMgr::TopologyPendingAckCount() const -> std::size_t {
  return pending_geometry_broadcasts_.size();
}

auto CellAppMgr::BuildTopologyFingerprint() const -> std::string {
  if (spaces_.empty()) return "spaces=0";
  std::vector<const SpacePartition*> partitions;
  partitions.reserve(spaces_.size());
  for (const auto& [_, partition] : spaces_) partitions.push_back(&partition);
  std::sort(partitions.begin(), partitions.end(),
            [](const SpacePartition* a, const SpacePartition* b) {
              return a->space_id < b->space_id;
            });

  std::string out = std::format("spaces={}", partitions.size());
  for (const auto* partition : partitions) {
    auto leaves = partition->bsp.Leaves();
    std::sort(leaves.begin(), leaves.end(), [](const CellInfo* a, const CellInfo* b) {
      return a->cell_id < b->cell_id;
    });
    const auto pending = std::count_if(
        pending_geometry_broadcasts_.begin(), pending_geometry_broadcasts_.end(),
        [space_id = partition->space_id](const PendingGeometryBroadcast& p) {
          return p.space_id == space_id;
        });
    out += std::format(" | space={} version={} freeze_epoch={} leaves={} primary={} "
                       "pending_ack={}",
                       partition->space_id, partition->geometry_version,
                       partition->freeze_epoch, leaves.size(), partition->bsp.PrimaryCellId(),
                       pending);
    for (const auto* leaf : leaves) {
      out += std::format(" cell={} app={} bounds=({:.1f},{:.1f},{:.1f},{:.1f})",
                         leaf->cell_id, AppIdForAddress(leaf->cellapp_addr),
                         leaf->bounds.min_x, leaf->bounds.min_z, leaf->bounds.max_x,
                         leaf->bounds.max_z);
    }
  }
  return out;
}

auto CellAppMgr::BuildRetireStatusSummary() const -> std::string {
  std::vector<const CellAppInfo*> apps;
  apps.reserve(cellapps_.size());
  for (const auto& [_, info] : cellapps_) {
    if (info.is_retiring) apps.push_back(&info);
  }
  std::sort(apps.begin(), apps.end(), [](const CellAppInfo* a, const CellAppInfo* b) {
    return a->app_id < b->app_id;
  });
  if (apps.empty()) return "retiring=0";

  const auto now = Clock::now();
  std::string out = std::format("retiring={}", apps.size());
  for (const auto* app : apps) {
    const auto addr = app->internal_addr;
    std::size_t owned = 0;
    for (const auto& [_, partition] : spaces_) {
      for (const auto* leaf : partition.bsp.Leaves()) {
        if (leaf->cellapp_addr == addr) ++owned;
      }
    }
    const auto drains = static_cast<std::size_t>(
        std::count_if(retire_drains_.begin(), retire_drains_.end(),
                      [&](const RetireDrain& drain) {
                        return drain.source_addr == addr || drain.target_addr == addr;
                      }));
    const auto pending = static_cast<std::size_t>(
        std::count_if(pending_geometry_broadcasts_.begin(),
                      pending_geometry_broadcasts_.end(),
                      [&](const PendingGeometryBroadcast& pending) {
                        if (pending.awaiting_addr == addr) return true;
                        return std::any_of(pending.extra_recipients.begin(),
                                           pending.extra_recipients.end(),
                                           [&](const ExtraGeometryRecipient& extra) {
                                             return extra.addr == addr;
                                           });
                      }));
    const auto stuck = static_cast<std::size_t>(
        std::count_if(retire_drains_.begin(), retire_drains_.end(),
                      [&](const RetireDrain& drain) {
                        if (drain.source_addr != addr && drain.target_addr != addr) return false;
                        return IsRetireDrainStuck(drain, now);
                      }));
    const bool ready = owned == 0 && drains == 0 && pending == 0;
    out += std::format(" app={} owned={} drains={} pending={} ready={} stuck={}",
                       app->app_id, owned, drains, pending, ready ? 1 : 0, stuck);
  }
  return out;
}

auto CellAppMgr::RecoveryWindowActive() const -> bool {
  // Mirrors BigWorld startRecovery: a freshly (re)started mgr holds topology
  // mutation until recovery_deadline_ so live workers can report their BSP
  // (OnRecoverCellAppState) before we balance / split against a partial view.
  // Set in Init from startup_quiescence_window_; stays unset (open) for
  // harness tests that don't run Init.
  return recovery_deadline_ != TimePoint{} && Clock::now() < recovery_deadline_;
}

auto CellAppMgr::BuildLbDecisionSummary() const -> std::string {
  return FormatLbDecision(last_lb_decision_);
}

auto CellAppMgr::BuildLbDecisionHistorySummary() const -> std::string {
  if (lb_decision_history_.empty()) return "decisions=0";
  std::string out = std::format("decisions={}", lb_decision_history_.size());
  for (const auto& decision : lb_decision_history_) {
    out += " | ";
    out += FormatLbDecision(decision);
  }
  return out;
}

auto CellAppMgr::FormatLbDecision(const LbDecision& decision) const -> std::string {
  std::string out =
      std::format("seq={} tick={} action={} reason={} space={} cell={} target_cell={} "
                  "source_app={} target_app={} version={}",
                  decision.sequence, decision.tick, decision.action, decision.reason,
                  decision.space_id, decision.cell_id, decision.target_cell_id,
                  decision.source_app_id, decision.target_app_id, decision.geometry_version);
  if (!decision.detail.empty()) out += " detail=" + decision.detail;
  return out;
}

auto CellAppMgr::SnapshotLeafTopology(const SpacePartition& partition,
                                      const Address& app_id_override_addr,
                                      uint32_t app_id_override) const
    -> std::vector<LeafTopologySnapshot> {
  std::vector<LeafTopologySnapshot> out;
  const auto leaves = partition.bsp.Leaves();
  out.reserve(leaves.size());
  for (const auto* leaf : leaves) {
    const auto app_id = leaf->cellapp_addr == app_id_override_addr && app_id_override != 0
                            ? app_id_override
                            : AppIdForAddress(leaf->cellapp_addr);
    out.push_back(LeafTopologySnapshot{
        leaf->cell_id, app_id, leaf->load, leaf->entity_count, leaf->bounds});
  }
  std::sort(out.begin(), out.end(), [](const LeafTopologySnapshot& a,
                                       const LeafTopologySnapshot& b) {
    return a.cell_id < b.cell_id;
  });
  return out;
}

auto CellAppMgr::FormatBoundsForDecision(const CellBounds& bounds) -> std::string {
  return std::format("({:.1f}/{:.1f}/{:.1f}/{:.1f})", bounds.min_x, bounds.min_z,
                     bounds.max_x, bounds.max_z);
}

auto CellAppMgr::LeafTopologyEqual(const LeafTopologySnapshot& a,
                                   const LeafTopologySnapshot& b) -> bool {
  return a.cell_id == b.cell_id && a.app_id == b.app_id && a.load == b.load &&
         a.entity_count == b.entity_count && a.bounds.min_x == b.bounds.min_x &&
         a.bounds.min_z == b.bounds.min_z && a.bounds.max_x == b.bounds.max_x &&
         a.bounds.max_z == b.bounds.max_z;
}

auto CellAppMgr::FormatLeafTopologyChange(const LeafTopologySnapshot* before,
                                          const LeafTopologySnapshot* after) -> std::string {
  const auto cell_id = before != nullptr ? before->cell_id : after->cell_id;
  const auto before_app = before != nullptr ? std::to_string(before->app_id) : "missing";
  const auto after_app = after != nullptr ? std::to_string(after->app_id) : "missing";
  const auto before_load = before != nullptr ? std::format("{:.3f}", before->load) : "missing";
  const auto after_load = after != nullptr ? std::format("{:.3f}", after->load) : "missing";
  const auto before_entities =
      before != nullptr ? std::to_string(before->entity_count) : "missing";
  const auto after_entities = after != nullptr ? std::to_string(after->entity_count) : "missing";
  const auto before_bounds =
      before != nullptr ? FormatBoundsForDecision(before->bounds) : "missing";
  const auto after_bounds = after != nullptr ? FormatBoundsForDecision(after->bounds) : "missing";
  return std::format("{}{{app:{}->{};load:{}->{};entities:{}->{};bounds:{}->{}}}", cell_id,
                     before_app, after_app, before_load, after_load, before_entities,
                     after_entities, before_bounds, after_bounds);
}

auto CellAppMgr::AppendTopologyDiff(std::string detail, const SpacePartition& partition,
                                    uint64_t before_version,
                                    const std::vector<LeafTopologySnapshot>& before) const
    -> std::string {
  const auto after = SnapshotLeafTopology(partition);
  if (!detail.empty()) detail += ",";
  detail += std::format("before_leaves={},after_leaves={},before_version={},after_version={}",
                        before.size(), after.size(), before_version, partition.geometry_version);

  std::size_t before_index = 0;
  std::size_t after_index = 0;
  std::size_t leaf_changes = 0;
  std::string leaf_diff;
  while (before_index < before.size() || after_index < after.size()) {
    const LeafTopologySnapshot* before_leaf = nullptr;
    const LeafTopologySnapshot* after_leaf = nullptr;
    if (after_index >= after.size() ||
        (before_index < before.size() && before[before_index].cell_id <
                                            after[after_index].cell_id)) {
      before_leaf = &before[before_index++];
    } else if (before_index >= before.size() ||
               after[after_index].cell_id < before[before_index].cell_id) {
      after_leaf = &after[after_index++];
    } else {
      before_leaf = &before[before_index++];
      after_leaf = &after[after_index++];
      if (LeafTopologyEqual(*before_leaf, *after_leaf)) continue;
    }

    ++leaf_changes;
    if (leaf_changes <= kLbDecisionLeafDiffLimit) {
      if (!leaf_diff.empty()) leaf_diff += ";";
      leaf_diff += FormatLeafTopologyChange(before_leaf, after_leaf);
    }
  }
  detail += std::format(",leaf_changes={}", leaf_changes);
  if (!leaf_diff.empty()) detail += ",leaf_diff=" + leaf_diff;
  if (leaf_changes > kLbDecisionLeafDiffLimit) {
    detail += std::format(",leaf_diff_truncated={}", leaf_changes - kLbDecisionLeafDiffLimit);
  }
  return detail;
}

auto CellAppMgr::AssignableCellAppCount() const -> std::size_t {
  const auto now = Clock::now();
  return static_cast<std::size_t>(
      std::count_if(cellapps_.begin(), cellapps_.end(),
                    [&](const auto& entry) {
                      return IsAssignableForLb(entry.second, now);
                    }));
}

auto CellAppMgr::StaleLoadReportCount() const -> std::size_t {
  const auto now = Clock::now();
  return static_cast<std::size_t>(
      std::count_if(cellapps_.begin(), cellapps_.end(), [&](const auto& entry) {
        return IsLoadReportStale(entry.second, now);
      }));
}

auto CellAppMgr::RetiringCellAppCount() const -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(cellapps_.begin(), cellapps_.end(),
                    [](const auto& entry) { return entry.second.is_retiring; }));
}

auto CellAppMgr::RetireDrainCount() const -> std::size_t {
  return retire_drains_.size();
}

auto CellAppMgr::RetireStuckDrainCount() const -> std::size_t {
  const auto now = Clock::now();
  return static_cast<std::size_t>(
      std::count_if(retire_drains_.begin(), retire_drains_.end(),
                    [&](const RetireDrain& drain) {
                      return IsRetireDrainStuck(drain, now);
                    }));
}

auto CellAppMgr::RetiringAppIdForWatcher() const -> uint32_t {
  for (const auto& [_, info] : cellapps_) {
    if (info.app_id == last_retire_app_id_ && info.is_retiring) return last_retire_app_id_;
  }
  for (const auto& [_, info] : cellapps_) {
    if (info.is_retiring) return info.app_id;
  }
  return 0;
}

auto CellAppMgr::SetRetiringAppId(uint32_t app_id) -> bool {
  if (app_id == 0) {
    for (auto& [_, info] : cellapps_) {
      info.is_retiring = false;
    }
    last_retire_app_id_ = 0;
    return true;
  }
  for (auto& [_, info] : cellapps_) {
    if (info.app_id != app_id) continue;
    info.is_retiring = true;
    last_retire_app_id_ = app_id;
    return true;
  }
  return false;
}

auto CellAppMgr::HandleRetireDrainReport(
    const Address& source_addr, const cellappmgr::InformCellLoad::CellReport& rep) -> bool {
  auto it = std::find_if(retire_drains_.begin(), retire_drains_.end(),
                         [&](const RetireDrain& drain) {
                           return drain.source_addr == source_addr && drain.cell_id == rep.cell_id;
                         });
  if (it == retire_drains_.end()) return false;

  if (rep.entity_count != it->last_entity_count) it->last_progress_at = Clock::now();
  it->last_entity_count = rep.entity_count;
  if (rep.entity_count != 0) return true;

  if (auto host_it = cellapps_.find(source_addr); host_it != cellapps_.end()) {
    SendRemoveCell(host_it->second, it->space_id, it->cell_id);
  }
  uint64_t geometry_version = 0;
  if (auto sp_it = spaces_.find(it->space_id); sp_it != spaces_.end()) {
    geometry_version = sp_it->second.geometry_version;
  }
  const auto before_drains = retire_drains_.size();
  RecordLbDecision("drain-complete", "retire-drain", it->space_id, it->cell_id, 0,
                   AppIdForAddress(source_addr), 0, geometry_version,
                   std::format("entities=0,before_drains={},after_drains={},before_version={},"
                               "after_version={}",
                               before_drains, before_drains - 1, geometry_version,
                               geometry_version));
  ATLAS_LOG_INFO("CellAppMgr: retire drain completed space={} cell={}", it->space_id,
                 it->cell_id);
  retire_drains_.erase(it);
  return true;
}

auto CellAppMgr::PickAlternateHost(const Address& exclude_addr) const -> const CellAppInfo* {
  // Prefer a survivor on a different machine; otherwise choose least-loaded.
  const CellAppInfo* best_diff_ip = nullptr;
  const CellAppInfo* best_any = nullptr;
  const auto now = Clock::now();
  for (const auto& [addr, info] : cellapps_) {
    if (!IsAssignableForLb(info, now)) continue;
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

auto CellAppMgr::PickAlternateHostInSpace(const Address& exclude_addr,
                                          const SpacePartition& partition) const
    -> const CellAppInfo* {
  const CellAppInfo* best = nullptr;
  const auto now = Clock::now();
  for (const auto* leaf : partition.bsp.Leaves()) {
    if (leaf->cellapp_addr == exclude_addr) continue;
    auto it = cellapps_.find(leaf->cellapp_addr);
    if (it == cellapps_.end()) continue;
    if (!IsAssignableForLb(it->second, now)) continue;
    if (best == nullptr || it->second.load < best->load ||
        (it->second.load == best->load && it->second.app_id < best->app_id)) {
      best = &it->second;
    }
  }
  return best;
}

void CellAppMgr::SendAddCell(const CellAppInfo& target, SpaceID space_id,
                             cellappmgr::CellID cell_id, const CellBounds& bounds, bool is_primary,
                             const std::string& space_master_type,
                             const Address& space_data_source_addr) {
  if (target.channel == nullptr) {
    ATLAS_LOG_WARNING("CellAppMgr: AddCellToSpace skipped — no channel to app_id={}",
                      target.app_id);
    return;
  }
  cellappmgr::AddCellToSpace msg;
  msg.space_id = space_id;
  msg.cell_id = cell_id;
  msg.bounds = bounds;
  msg.is_primary = is_primary;
  msg.space_master_type = space_master_type;
  msg.space_data_source_addr = space_data_source_addr;
  (void)target.channel->SendMessage(msg);
}

void CellAppMgr::SendRemoveCell(const CellAppInfo& target, SpaceID space_id,
                                cellappmgr::CellID cell_id) {
  if (target.channel == nullptr) {
    ATLAS_LOG_WARNING("CellAppMgr: RemoveCellFromSpace skipped — no channel to app_id={}",
                      target.app_id);
    return;
  }
  cellappmgr::RemoveCellFromSpace msg;
  msg.space_id = space_id;
  msg.cell_id = cell_id;
  (void)target.channel->SendMessage(msg);
}

void CellAppMgr::BroadcastGeometry(
    SpacePartition& partition, std::span<const ExtraGeometryRecipient> extra_recipients) {
  BinaryWriter w;
  partition.bsp.Serialize(w);
  auto blob = w.Detach();
  const bool topology_changed = blob != partition.last_broadcast_blob;

  baseapp::SpaceBspGeometry notice;
  std::vector<std::byte> debug_blob;
  bool debug_changed = false;
  if (baseapps_.empty()) partition.last_debug_geometry_baseapp_count = 0;
  if (!baseapps_.empty()) {
    notice.space_id = partition.space_id;
    for (const auto* ci : partition.bsp.Leaves()) {
      uint8_t owner_index = 0;
      if (auto cellapp_it = cellapps_.find(ci->cellapp_addr); cellapp_it != cellapps_.end()) {
        owner_index = static_cast<uint8_t>(std::min<uint32_t>(cellapp_it->second.app_id, 255u));
      }
      notice.leaves.push_back({ci->cell_id, owner_index, ci->bounds.min_x, ci->bounds.min_z,
                               ci->bounds.max_x, ci->bounds.max_z, ci->load, ci->entity_count});
    }
    BinaryWriter debug_writer;
    notice.Serialize(debug_writer);
    debug_blob = debug_writer.Detach();
    debug_changed = debug_blob != partition.last_debug_geometry_blob ||
                    baseapps_.size() != partition.last_debug_geometry_baseapp_count;
  }

  if (!topology_changed && !debug_changed) return;

  if (topology_changed) {
    ++partition.geometry_version;
    ++partition.freeze_epoch;
    cellappmgr::UpdateGeometry msg;
    msg.space_id = partition.space_id;
    msg.geometry_version = partition.geometry_version;
    msg.bsp_blob.assign(blob.begin(), blob.end());

    std::unordered_map<Address, bool> hosts;
    for (const auto* ci : partition.bsp.Leaves()) hosts[ci->cellapp_addr] = true;
    for (const auto& extra : extra_recipients) hosts[extra.addr] = true;

    auto fan_should_offload = [&](bool enable) {
      auto send = [&](const Address& addr, cellappmgr::CellID cell_id) {
        auto it = cellapps_.find(addr);
        if (it == cellapps_.end() || it->second.channel == nullptr) return;
        cellappmgr::ShouldOffload so;
        so.space_id = partition.space_id;
        so.cell_id = cell_id;
        so.enable = enable;
        so.freeze_epoch = partition.freeze_epoch;
        (void)it->second.channel->SendMessage(so);
      };
      for (const auto* ci : partition.bsp.Leaves()) {
        send(ci->cellapp_addr, ci->cell_id);
      }
      for (const auto& extra : extra_recipients) send(extra.addr, extra.cell_id);
    };
    fan_should_offload(false);

    for (const auto& [addr, _] : hosts) {
      auto it = cellapps_.find(addr);
      if (it == cellapps_.end() || it->second.channel == nullptr) continue;
      (void)it->second.channel->SendMessage(msg);
    }

    fan_should_offload(true);
    partition.last_broadcast_blob = std::move(blob);
  }

  if (debug_changed) {
    bool sent = false;
    for (const auto& [_, ch] : baseapps_) {
      if (ch == nullptr) continue;
      sent = true;
      (void)ch->SendMessage(notice);
    }
    if (sent) {
      partition.last_debug_geometry_blob = std::move(debug_blob);
      partition.last_debug_geometry_baseapp_count = baseapps_.size();
    }
  }
}

}  // namespace atlas
