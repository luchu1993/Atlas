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
#include "server/snapshot_envelope.h"
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
ServerAppOption<uint32_t> s_ha_reattach_watchdog_ms{
    30000u, "cellappmgr_ha_reattach_watchdog_ms",
    "cellappmgr/ha/reattach_watchdog_ms", WatcherMode::kReadWrite};

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
  return !info.needs_reattach && now - LastLoadReportAt(info) >= LoadReportStaleWindow();
}

auto HasFreshLoadReport(const CellAppMgr::CellAppInfo& info, TimePoint now) -> bool {
  return !info.needs_reattach && !IsLoadReportStale(info, now);
}

auto IsAssignableForLb(const CellAppMgr::CellAppInfo& info, TimePoint now) -> bool {
  return !info.is_retiring && HasFreshLoadReport(info, now);
}

auto ReattachWatchdogWindow() -> Duration {
  return Milliseconds{s_ha_reattach_watchdog_ms.Value()};
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

constexpr uint32_t kSnapshotMagic = 0x314D4143u;
constexpr uint32_t kSnapshotVersion = 5;
// 1 GiB ceiling — per-cell load buckets + pending-geometry blobs scale
// with BSP topology; headroom for ~256 cellapps × ~64 cells/space.
constexpr uint64_t kMaxSnapshotPayloadBytes = 1024ull * 1024ull * 1024ull;
constexpr uint64_t kMaxSnapshotFileBytes =
    kMaxSnapshotPayloadBytes + snapshot_envelope::kEnvelopeBytes;
constexpr uint32_t kMaxSnapshotEntries = 1024 * 1024;
constexpr uint32_t kMaxSnapshotBlobBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxSnapshotStringBytes = 64 * 1024;
constexpr std::string_view kSnapshotModuleName = "CellAppMgr";

using snapshot_envelope::PayloadView;
using snapshot_envelope::FileReadiness;
using snapshot_envelope::WatcherErrorDetail;

auto SnapshotPayload(std::span<const std::byte> bytes) -> Result<PayloadView> {
  return snapshot_envelope::ReadPayload(bytes, kSnapshotMagic, kSnapshotVersion,
                                        kMaxSnapshotPayloadBytes, kSnapshotModuleName);
}

auto SnapshotBackupPath(const std::filesystem::path& path) -> std::filesystem::path {
  return snapshot_envelope::BackupPath(path);
}

auto SnapshotTempPath(const std::filesystem::path& path) -> std::filesystem::path {
  return snapshot_envelope::TempPath(path);
}

auto SnapshotFileReadinessForPath(const std::filesystem::path& path, bool validate)
    -> FileReadiness {
  return snapshot_envelope::Readiness(path, validate, kSnapshotMagic, kSnapshotVersion,
                                      kMaxSnapshotFileBytes, kMaxSnapshotPayloadBytes,
                                      kSnapshotModuleName);
}

auto PreserveSnapshotBackup(const std::filesystem::path& path) -> Result<void> {
  return snapshot_envelope::PreserveBackup(path, kSnapshotMagic, kSnapshotVersion,
                                           kMaxSnapshotPayloadBytes, kSnapshotModuleName);
}

void WriteAddress(BinaryWriter& w, const Address& addr) {
  w.Write(addr.Ip());
  w.Write(addr.Port());
}

auto ReadAddress(BinaryReader& r, const char* context) -> Result<Address> {
  auto ip = r.Read<uint32_t>();
  auto port = r.Read<uint16_t>();
  if (!ip || !port) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} address truncated", context)};
  }
  return Address(*ip, *port);
}

auto ReadCount(BinaryReader& r, const char* context) -> Result<uint32_t> {
  auto count = r.ReadPackedInt();
  if (!count) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} count truncated", context)};
  }
  if (*count > kMaxSnapshotEntries) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} count too large", context)};
  }
  return *count;
}

void WriteBlob(BinaryWriter& w, std::span<const std::byte> blob) {
  w.WritePackedInt(static_cast<uint32_t>(blob.size()));
  w.WriteBytes(blob);
}

auto ReadBlob(BinaryReader& r, const char* context) -> Result<std::vector<std::byte>> {
  auto size = r.ReadPackedInt();
  if (!size) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} size truncated", context)};
  }
  if (*size > kMaxSnapshotBlobBytes) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} blob too large", context)};
  }
  auto bytes = r.ReadBytes(*size);
  if (!bytes) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} blob truncated", context)};
  }
  return std::vector<std::byte>(bytes->begin(), bytes->end());
}

auto ReadSnapshotString(BinaryReader& r, const char* context) -> Result<std::string> {
  auto size = r.ReadPackedInt();
  if (!size) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} string truncated", context)};
  }
  if (*size > kMaxSnapshotStringBytes) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} string too large", context)};
  }
  auto bytes = r.ReadBytes(*size);
  if (!bytes) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CellAppMgr snapshot: {} string body truncated", context)};
  }
  return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
}

void WriteBuckets(BinaryWriter& w, const LoadBuckets& buckets) {
  for (uint32_t bucket : buckets) w.Write(bucket);
}

void WriteLoadBuckets(BinaryWriter& w, const LoadCostBuckets& buckets) {
  for (uint64_t bucket : buckets) w.Write(bucket);
}

auto ReadBuckets(BinaryReader& r, const char* context) -> Result<LoadBuckets> {
  LoadBuckets buckets{};
  for (auto& bucket : buckets) {
    auto value = r.Read<uint32_t>();
    if (!value) {
      return Error{ErrorCode::kInvalidArgument,
                   std::format("CellAppMgr snapshot: {} buckets truncated", context)};
    }
    bucket = *value;
  }
  return buckets;
}

auto ReadLoadBuckets(BinaryReader& r, const char* context) -> Result<LoadCostBuckets> {
  LoadCostBuckets buckets{};
  for (auto& bucket : buckets) {
    auto value = r.Read<uint64_t>();
    if (!value) {
      return Error{ErrorCode::kInvalidArgument,
                   std::format("CellAppMgr snapshot: {} load buckets truncated", context)};
    }
    bucket = *value;
  }
  return buckets;
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

  if (!Config().snapshot_path.empty()) {
    if (auto restore = RestoreSnapshotFromFile(Config().snapshot_path); restore) {
      ATLAS_LOG_INFO("CellAppMgr: restored HA snapshot source={} path={}",
                     last_snapshot_restore_source_, last_snapshot_restore_path_.string());
    } else if (restore.Error().Code() == ErrorCode::kNotFound) {
      ATLAS_LOG_INFO("CellAppMgr: HA snapshot restore skipped: {}",
                     restore.Error().Message());
    } else {
      ATLAS_LOG_ERROR("CellAppMgr: HA snapshot restore failed: {}", restore.Error().Message());
      return false;
    }
  }

  // Bump + persist BEFORE any handler dispatches. Advertising gen=N+1
  // without first persisting it would let a crash + restart re-issue the
  // same value across two distinct mgr lifetimes — the synchronous
  // SaveSnapshotToFile below closes that window. See P1-A1.
  ++mgr_generation_;
  MarkSnapshotDirty("mgr-generation-bump");
  if (!Config().snapshot_path.empty()) {
    auto save = SaveSnapshotToFile(Config().snapshot_path);
    if (!save) {
      ATLAS_LOG_ERROR("CellAppMgr: failed to persist mgr_generation={} on Init: {}",
                      mgr_generation_, save.Error().Message());
      return false;
    }
  } else {
    ATLAS_LOG_WARNING(
        "CellAppMgr: mgr_generation={} not persisted (no --snapshot-path); "
        "monotonicity across restarts cannot be guaranteed",
        mgr_generation_);
  }
  ATLAS_LOG_INFO("CellAppMgr: mgr_generation={}", mgr_generation_);

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

  ATLAS_LOG_INFO("CellAppMgr: initialised");
  return true;
}

void CellAppMgr::Fini() {
  SaveConfiguredSnapshot("shutdown");
  ManagerApp::Fini();
}

auto CellAppMgr::Snapshot() const -> std::vector<std::byte> {
  BinaryWriter payload_writer;
  payload_writer.Write(mgr_generation_);
  payload_writer.Write(next_cellapp_app_id_);
  payload_writer.Write(next_cell_id_);
  payload_writer.Write(last_balance_tick_);
  payload_writer.Write(last_retire_app_id_);

  payload_writer.WritePackedInt(static_cast<uint32_t>(cellapps_.size()));
  for (const auto& [addr, info] : cellapps_) {
    WriteAddress(payload_writer, addr);
    payload_writer.Write(info.app_id);
    payload_writer.Write(info.load);
    payload_writer.Write(info.entity_count);
    payload_writer.Write<uint8_t>(info.is_retiring ? 1u : 0u);
    payload_writer.Write<uint8_t>(info.needs_reattach ? 1u : 0u);
  }

  payload_writer.WritePackedInt(static_cast<uint32_t>(spaces_.size()));
  for (const auto& [space_id, partition] : spaces_) {
    payload_writer.Write(space_id);
    payload_writer.Write(partition.geometry_version);
    payload_writer.Write(partition.freeze_epoch);
    payload_writer.WriteString(partition.space_master_type);
    BinaryWriter bsp_writer;
    partition.bsp.Serialize(bsp_writer);
    const auto bsp_blob = bsp_writer.Detach();
    WriteBlob(payload_writer, bsp_blob);
    WriteBlob(payload_writer, partition.last_broadcast_blob);
    WriteBlob(payload_writer, partition.last_debug_geometry_blob);
    payload_writer.Write(static_cast<uint64_t>(partition.last_debug_geometry_baseapp_count));
  }

  payload_writer.WritePackedInt(static_cast<uint32_t>(cell_distributions_.size()));
  for (const auto& [cell_id, dist] : cell_distributions_) {
    payload_writer.Write(cell_id);
    payload_writer.Write(dist.entity_count);
    payload_writer.Write(dist.median_x);
    payload_writer.Write(dist.median_z);
    payload_writer.Write(dist.weighted_load);
    payload_writer.Write(dist.tick_load);
    payload_writer.Write(dist.script_tick_us);
    payload_writer.Write(dist.native_tick_us);
    payload_writer.Write(dist.witness_count);
    payload_writer.Write(dist.aoi_peer_count);
    payload_writer.Write(dist.aoi_reliable_bytes);
    payload_writer.Write(dist.aoi_unreliable_bytes);
    payload_writer.Write(dist.backup_bytes);
    WriteBuckets(payload_writer, dist.x_buckets);
    WriteBuckets(payload_writer, dist.z_buckets);
    WriteLoadBuckets(payload_writer, dist.x_load_buckets);
    WriteLoadBuckets(payload_writer, dist.z_load_buckets);
  }

  auto write_ticks = [&payload_writer](const auto& ticks) {
    payload_writer.WritePackedInt(static_cast<uint32_t>(ticks.size()));
    for (const auto& [cell_id, value] : ticks) {
      payload_writer.Write(cell_id);
      payload_writer.Write(value);
    }
  };
  write_ticks(hot_leaf_balance_ticks_);
  write_ticks(idle_leaf_balance_ticks_);

  payload_writer.WritePackedInt(static_cast<uint32_t>(pending_geometry_broadcasts_.size()));
  for (const auto& pending : pending_geometry_broadcasts_) {
    payload_writer.Write(pending.space_id);
    payload_writer.Write(pending.awaiting_cell_id);
    WriteAddress(payload_writer, pending.awaiting_addr);
    payload_writer.Write<uint8_t>(pending.allow_timeout_broadcast ? 1u : 0u);
    payload_writer.WritePackedInt(static_cast<uint32_t>(pending.extra_recipients.size()));
    for (const auto& extra : pending.extra_recipients) {
      WriteAddress(payload_writer, extra.addr);
      payload_writer.Write(extra.cell_id);
    }
  }

  payload_writer.WritePackedInt(static_cast<uint32_t>(retire_drains_.size()));
  for (const auto& drain : retire_drains_) {
    payload_writer.Write(drain.space_id);
    payload_writer.Write(drain.cell_id);
    WriteAddress(payload_writer, drain.source_addr);
    WriteAddress(payload_writer, drain.target_addr);
    payload_writer.Write(drain.last_entity_count);
    payload_writer.Write<uint8_t>(drain.geometry_published ? 1u : 0u);
  }

  const auto payload = payload_writer.Detach();
  return snapshot_envelope::WrapPayload(
      std::span<const std::byte>(payload.data(), payload.size()), kSnapshotMagic,
      kSnapshotVersion);
}

auto CellAppMgr::Restore(std::span<const std::byte> bytes) -> Result<void> {
  auto snapshot_payload = SnapshotPayload(bytes);
  if (!snapshot_payload) return snapshot_payload.Error();

  BinaryReader r(snapshot_payload->payload);
  auto saved_generation = r.Read<uint64_t>();
  auto next_app = r.Read<uint32_t>();
  auto next_cell = r.Read<cellappmgr::CellID>();
  auto last_balance = r.Read<uint64_t>();
  auto last_retire = r.Read<uint32_t>();
  if (!saved_generation || !next_app || !next_cell || !last_balance || !last_retire) {
    return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: header truncated"};
  }

  const auto now = Clock::now();
  std::unordered_map<Address, CellAppInfo> restored_cellapps;
  std::unordered_set<uint32_t> restored_app_ids;
  uint32_t max_app_id = 0;
  auto cellapp_count = ReadCount(r, "cellapps");
  if (!cellapp_count) return cellapp_count.Error();
  for (uint32_t i = 0; i < *cellapp_count; ++i) {
    auto addr = ReadAddress(r, "cellapp");
    auto app_id = r.Read<uint32_t>();
    auto load = r.Read<float>();
    auto entity_count = r.Read<uint32_t>();
    auto retiring = r.Read<uint8_t>();
    auto needs_reattach = r.Read<uint8_t>();
    if (!addr || !app_id || !load || !entity_count || !retiring || !needs_reattach) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: cellapp truncated"};
    }
    if (*app_id == 0 || *app_id > kMaxCellAppAppId) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad cellapp app_id"};
    }
    if (!std::isfinite(*load)) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad cellapp load"};
    }
    if (*retiring > 1 || *needs_reattach > 1) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad cellapp flags"};
    }
    if (!restored_app_ids.insert(*app_id).second) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: duplicate app_id"};
    }
    CellAppInfo info;
    info.internal_addr = *addr;
    info.app_id = *app_id;
    info.load = *load;
    info.entity_count = *entity_count;
    info.registered_at = now;
    info.last_load_report_at = now;
    info.is_retiring = *retiring != 0;
    info.needs_reattach = true;
    info.restored_from_snapshot = true;
    max_app_id = std::max(max_app_id, info.app_id);
    if (!restored_cellapps.emplace(*addr, info).second) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: duplicate cellapp"};
    }
  }

  std::unordered_map<SpaceID, SpacePartition> restored_spaces;
  std::unordered_set<uint64_t> restored_leaf_ids;
  cellappmgr::CellID max_cell_id = 0;
  auto space_count = ReadCount(r, "spaces");
  if (!space_count) return space_count.Error();
  for (uint32_t i = 0; i < *space_count; ++i) {
    auto space_id = r.Read<uint32_t>();
    auto geometry_version = r.Read<uint64_t>();
    auto freeze_epoch = r.Read<uint64_t>();
    auto master_type = ReadSnapshotString(r, "space master type");
    auto bsp_blob = ReadBlob(r, "bsp");
    auto last_broadcast = ReadBlob(r, "last_broadcast");
    auto debug_geometry = ReadBlob(r, "debug_geometry");
    auto debug_baseapps = r.Read<uint64_t>();
    if (!space_id || !geometry_version || !freeze_epoch || !master_type || !bsp_blob ||
        !last_broadcast || !debug_geometry || !debug_baseapps) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: space truncated"};
    }
    if (*space_id == kInvalidSpaceID) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad space id"};
    }
    BinaryReader bsp_reader(std::span<const std::byte>(bsp_blob->data(), bsp_blob->size()));
    auto bsp = BSPTree::Deserialize(bsp_reader);
    if (!bsp) return bsp.Error();
    const auto leaves = bsp->Leaves();
    if (leaves.empty() || bsp->FindCellById(bsp->PrimaryCellId()) == nullptr) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad space bsp"};
    }
    for (const auto* leaf : leaves) {
      if (leaf->cell_id == 0) {
        return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad leaf cell_id"};
      }
      if (!restored_leaf_ids.insert(leaf->cell_id).second) {
        return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: duplicate leaf cell_id"};
      }
      if (!restored_cellapps.contains(leaf->cellapp_addr)) {
        return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: unknown leaf cellapp"};
      }
      max_cell_id = std::max(max_cell_id, leaf->cell_id);
    }
    SpacePartition partition;
    partition.space_id = *space_id;
    partition.bsp = std::move(*bsp);
    partition.geometry_version = *geometry_version;
    partition.freeze_epoch = *freeze_epoch;
    partition.space_master_type = std::move(*master_type);
    partition.last_broadcast_blob = std::move(*last_broadcast);
    partition.last_debug_geometry_blob = std::move(*debug_geometry);
    partition.last_debug_geometry_baseapp_count = static_cast<std::size_t>(*debug_baseapps);
    if (!restored_spaces.emplace(*space_id, std::move(partition)).second) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: duplicate space"};
    }
  }

  auto pending_key = [](SpaceID space_id, cellappmgr::CellID cell_id) {
    return (static_cast<uint64_t>(space_id) << 32) | static_cast<uint64_t>(cell_id);
  };
  auto find_leaf = [&restored_spaces](SpaceID space_id, cellappmgr::CellID cell_id)
      -> const CellInfo* {
    const auto space_it = restored_spaces.find(space_id);
    if (space_it == restored_spaces.end()) return nullptr;
    return space_it->second.bsp.FindCellById(cell_id);
  };

  std::unordered_map<cellappmgr::CellID, CellDistribution> restored_distributions;
  auto dist_count = ReadCount(r, "cell distributions");
  if (!dist_count) return dist_count.Error();
  for (uint32_t i = 0; i < *dist_count; ++i) {
    auto cell_id = r.Read<cellappmgr::CellID>();
    auto entity_count = r.Read<uint32_t>();
    auto median_x = r.Read<float>();
    auto median_z = r.Read<float>();
    auto weighted_load = r.Read<float>();
    auto tick_load = r.Read<float>();
    auto script_tick_us = r.Read<uint64_t>();
    if (!cell_id || !entity_count || !median_x || !median_z || !weighted_load || !tick_load ||
        !script_tick_us) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: cell distribution truncated"};
    }
    auto native_tick_us = r.Read<uint64_t>();
    auto witness_count = r.Read<uint32_t>();
    auto aoi_peer_count = r.Read<uint32_t>();
    auto aoi_reliable_bytes = r.Read<uint64_t>();
    auto aoi_unreliable_bytes = r.Read<uint64_t>();
    auto backup_bytes = r.Read<uint64_t>();
    auto x_buckets = ReadBuckets(r, "x");
    auto z_buckets = ReadBuckets(r, "z");
    auto x_load_buckets = ReadLoadBuckets(r, "x");
    auto z_load_buckets = ReadLoadBuckets(r, "z");
    if (!native_tick_us || !witness_count || !aoi_peer_count || !aoi_reliable_bytes ||
        !aoi_unreliable_bytes || !backup_bytes || !x_buckets || !z_buckets ||
        !x_load_buckets || !z_load_buckets) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: cell distribution truncated"};
    }
    if (!restored_leaf_ids.contains(*cell_id)) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: unknown cell distribution"};
    }
    if (!std::isfinite(*median_x) || !std::isfinite(*median_z) ||
        !std::isfinite(*weighted_load) || !std::isfinite(*tick_load) ||
        *weighted_load < 0.f || *tick_load < 0.f) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad cell distribution"};
    }
    CellDistribution dist;
    dist.entity_count = *entity_count;
    dist.median_x = *median_x;
    dist.median_z = *median_z;
    dist.weighted_load = *weighted_load;
    dist.tick_load = *tick_load;
    dist.script_tick_us = *script_tick_us;
    dist.native_tick_us = *native_tick_us;
    dist.witness_count = *witness_count;
    dist.aoi_peer_count = *aoi_peer_count;
    dist.aoi_reliable_bytes = *aoi_reliable_bytes;
    dist.aoi_unreliable_bytes = *aoi_unreliable_bytes;
    dist.backup_bytes = *backup_bytes;
    dist.x_buckets = *x_buckets;
    dist.z_buckets = *z_buckets;
    dist.x_load_buckets = *x_load_buckets;
    dist.z_load_buckets = *z_load_buckets;
    if (!restored_distributions.emplace(*cell_id, dist).second) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: duplicate cell distribution"};
    }
  }

  auto read_ticks = [&r, &restored_leaf_ids](const char* context)
      -> Result<std::unordered_map<cellappmgr::CellID, uint32_t>> {
    std::unordered_map<cellappmgr::CellID, uint32_t> ticks;
    auto count = ReadCount(r, context);
    if (!count) return count.Error();
    for (uint32_t i = 0; i < *count; ++i) {
      auto cell_id = r.Read<cellappmgr::CellID>();
      auto value = r.Read<uint32_t>();
      if (!cell_id || !value) {
        return Error{ErrorCode::kInvalidArgument,
                      std::format("CellAppMgr snapshot: {} ticks truncated", context)};
      }
      if (!restored_leaf_ids.contains(*cell_id)) {
        return Error{ErrorCode::kInvalidArgument,
                     std::format("CellAppMgr snapshot: unknown {} tick cell", context)};
      }
      if (!ticks.emplace(*cell_id, *value).second) {
        return Error{ErrorCode::kInvalidArgument,
                     std::format("CellAppMgr snapshot: duplicate {} tick cell", context)};
      }
    }
    return ticks;
  };
  auto hot_ticks = read_ticks("hot");
  if (!hot_ticks) return hot_ticks.Error();
  auto idle_ticks = read_ticks("idle");
  if (!idle_ticks) return idle_ticks.Error();

  std::vector<PendingGeometryBroadcast> restored_pending;
  std::unordered_map<uint64_t, Address> restored_pending_targets;
  auto pending_count = ReadCount(r, "pending geometry");
  if (!pending_count) return pending_count.Error();
  for (uint32_t i = 0; i < *pending_count; ++i) {
    auto space_id = r.Read<uint32_t>();
    auto cell_id = r.Read<cellappmgr::CellID>();
    auto addr = ReadAddress(r, "pending geometry");
    auto allow_timeout = r.Read<uint8_t>();
    auto extra_count = ReadCount(r, "extra geometry recipients");
    if (!space_id || !cell_id || !addr || !allow_timeout || !extra_count) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: pending geometry truncated"};
    }
    if (*allow_timeout > 1) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: bad pending geometry flag"};
    }
    const auto* leaf = find_leaf(*space_id, *cell_id);
    if (leaf == nullptr) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: unknown pending geometry cell"};
    }
    if (!restored_cellapps.contains(*addr)) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: unknown pending geometry cellapp"};
    }
    if (leaf->cellapp_addr != *addr) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: pending geometry owner mismatch"};
    }
    if (!restored_pending_targets.emplace(pending_key(*space_id, *cell_id), *addr).second) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: duplicate pending geometry"};
    }
    PendingGeometryBroadcast pending;
    pending.space_id = *space_id;
    pending.awaiting_cell_id = *cell_id;
    pending.awaiting_addr = *addr;
    pending.sent_at = now;
    pending.allow_timeout_broadcast = *allow_timeout != 0;
    pending.extra_recipients.reserve(*extra_count);
    std::set<std::tuple<uint32_t, uint16_t, cellappmgr::CellID>> extra_keys;
    for (uint32_t j = 0; j < *extra_count; ++j) {
      auto extra_addr = ReadAddress(r, "extra geometry recipient");
      auto extra_cell = r.Read<cellappmgr::CellID>();
      if (!extra_addr || !extra_cell) {
        return Error{ErrorCode::kInvalidArgument,
                     "CellAppMgr snapshot: extra geometry recipient truncated"};
      }
      if (!restored_cellapps.contains(*extra_addr)) {
        return Error{ErrorCode::kInvalidArgument,
                     "CellAppMgr snapshot: unknown extra geometry cellapp"};
      }
      if (find_leaf(*space_id, *extra_cell) == nullptr) {
        return Error{ErrorCode::kInvalidArgument,
                     "CellAppMgr snapshot: unknown extra geometry cell"};
      }
      if (!extra_keys.emplace(extra_addr->Ip(), extra_addr->Port(), *extra_cell).second) {
        return Error{ErrorCode::kInvalidArgument,
                     "CellAppMgr snapshot: duplicate extra geometry recipient"};
      }
      pending.extra_recipients.push_back({*extra_addr, *extra_cell});
    }
    restored_pending.push_back(std::move(pending));
  }

  std::vector<RetireDrain> restored_retire_drains;
  std::set<std::tuple<SpaceID, cellappmgr::CellID, uint32_t, uint16_t, uint32_t, uint16_t>>
      restored_drain_keys;
  auto drain_count = ReadCount(r, "retire drains");
  if (!drain_count) return drain_count.Error();
  restored_retire_drains.reserve(*drain_count);
  for (uint32_t i = 0; i < *drain_count; ++i) {
    auto space_id = r.Read<uint32_t>();
    auto cell_id = r.Read<cellappmgr::CellID>();
    auto source_addr = ReadAddress(r, "retire drain source");
    auto target_addr = ReadAddress(r, "retire drain target");
    auto entity_count = r.Read<uint32_t>();
    auto geometry_published = r.Read<uint8_t>();
    if (!space_id || !cell_id || !source_addr || !target_addr || !entity_count ||
        !geometry_published) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: retire drain truncated"};
    }
    if (*geometry_published > 1) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: bad retire drain flag"};
    }
    if (*source_addr == *target_addr) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: bad retire drain endpoints"};
    }
    const auto source_it = restored_cellapps.find(*source_addr);
    const auto target_it = restored_cellapps.find(*target_addr);
    if (source_it == restored_cellapps.end() || target_it == restored_cellapps.end()) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: unknown retire drain cellapp"};
    }
    if (!source_it->second.is_retiring) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: retire drain source not retiring"};
    }
    const auto* leaf = find_leaf(*space_id, *cell_id);
    if (leaf == nullptr) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: unknown retire drain cell"};
    }
    if (leaf->cellapp_addr != *target_addr) {
      return Error{ErrorCode::kInvalidArgument,
                   "CellAppMgr snapshot: retire drain owner mismatch"};
    }
    const auto key = pending_key(*space_id, *cell_id);
    if (!*geometry_published) {
      const auto pending_it = restored_pending_targets.find(key);
      if (pending_it == restored_pending_targets.end() || pending_it->second != *target_addr) {
        return Error{ErrorCode::kInvalidArgument,
                     "CellAppMgr snapshot: retire drain missing pending geometry"};
      }
    }
    if (!restored_drain_keys
             .emplace(*space_id, *cell_id, source_addr->Ip(), source_addr->Port(),
                      target_addr->Ip(), target_addr->Port())
             .second) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: duplicate retire drain"};
    }
    RetireDrain drain;
    drain.space_id = *space_id;
    drain.cell_id = *cell_id;
    drain.source_addr = *source_addr;
    drain.target_addr = *target_addr;
    drain.last_entity_count = *entity_count;
    drain.started_at = now;
    drain.last_progress_at = now;
    drain.geometry_published = *geometry_published != 0;
    restored_retire_drains.push_back(drain);
  }
  if (r.Remaining() != 0) {
    return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: trailing bytes"};
  }

  for (auto& [_, partition] : restored_spaces) {
    for (auto* leaf : partition.bsp.LeavesMutable()) {
      auto dist_it = restored_distributions.find(leaf->cell_id);
      if (dist_it == restored_distributions.end()) continue;
      const auto& dist = dist_it->second;
      leaf->entity_count = dist.entity_count;
      leaf->load = dist.weighted_load;
      leaf->tick_load = dist.tick_load;
      leaf->script_tick_us = dist.script_tick_us;
      leaf->native_tick_us = dist.native_tick_us;
      leaf->witness_count = dist.witness_count;
      leaf->aoi_peer_count = dist.aoi_peer_count;
      leaf->aoi_reliable_bytes = dist.aoi_reliable_bytes;
      leaf->aoi_unreliable_bytes = dist.aoi_unreliable_bytes;
      leaf->backup_bytes = dist.backup_bytes;
      leaf->x_buckets = dist.x_buckets;
      leaf->z_buckets = dist.z_buckets;
      leaf->x_load_buckets = dist.x_load_buckets;
      leaf->z_load_buckets = dist.z_load_buckets;
    }
  }

  cellapps_ = std::move(restored_cellapps);
  spaces_ = std::move(restored_spaces);
  cell_distributions_ = std::move(restored_distributions);
  hot_leaf_balance_ticks_ = std::move(*hot_ticks);
  idle_leaf_balance_ticks_ = std::move(*idle_ticks);
  pending_geometry_broadcasts_ = std::move(restored_pending);
  retire_drains_ = std::move(restored_retire_drains);
  pending_space_creates_awaiting_cellapps_.clear();
  baseapps_.clear();
  next_cellapp_app_id_ = std::max(*next_app, max_app_id + 1);
  next_cell_id_ = std::max(*next_cell, static_cast<cellappmgr::CellID>(max_cell_id + 1));
  last_balance_tick_ = *last_balance;
  last_retire_app_id_ = *last_retire;
  mgr_generation_ = *saved_generation;
  snapshot_dirty_ = false;
  snapshot_dirty_at_ = {};
  snapshot_dirty_reason_.clear();
  return {};
}

auto CellAppMgr::SaveSnapshotToFile(const std::filesystem::path& path) -> Result<void> {
  if (path.empty()) return {};
  last_snapshot_attempt_at_ = Clock::now();
  auto record_error = [this, &path](const Error& error) -> Error {
    ++snapshot_save_failure_count_;
    ++snapshot_failure_count_;
    last_snapshot_save_path_ = path;
    last_snapshot_save_error_ = std::string(error.Message());
    return error;
  };
  if (auto parent = path.parent_path(); !parent.empty()) {
    auto create = fs::CreateDirectories(parent);
    if (!create) return record_error(create.Error());
  }

  const auto bytes = Snapshot();
  const auto topology = BuildTopologyFingerprint();
  const auto topology_pending_ack = TopologyPendingAckCount();
  if (bytes.size() > kMaxSnapshotFileBytes) {
    return record_error(
        Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: file too large"});
  }
  if (auto backup = PreserveSnapshotBackup(path); !backup) {
    if (backup.Error().Code() != ErrorCode::kInvalidArgument) {
      return record_error(backup.Error());
    }
    ++snapshot_backup_skip_count_;
    ATLAS_LOG_WARNING("CellAppMgr: HA snapshot backup skipped (.bak lags new main): {}",
                      backup.Error().Message());
  }
  auto tmp = SnapshotTempPath(path);
  auto write = fs::WriteFile(tmp, bytes);
  if (!write) {
    (void)fs::RemoveFile(tmp);
    return record_error(write.Error());
  }

  auto replace = fs::AtomicReplaceFile(tmp, path);
  if (!replace) {
    (void)fs::RemoveFile(tmp);
    return record_error(replace.Error());
  }
  last_snapshot_bytes_ = bytes.size();
  last_snapshot_save_at_ = Clock::now();
  last_snapshot_save_path_ = path;
  last_snapshot_save_topology_ = topology;
  last_snapshot_save_topology_pending_ack_ = topology_pending_ack;
  last_snapshot_save_error_.clear();
  snapshot_dirty_ = false;
  snapshot_dirty_at_ = {};
  snapshot_dirty_reason_.clear();
  ++snapshot_save_count_;

  const auto now = Clock::now();
  const auto decision =
      snapshot_envelope::EvaluateSizeWarning(SnapshotSizeHighWaterPct(), now,
                                             last_snapshot_size_warning_at_);
  if (decision.should_log) {
    last_snapshot_size_warning_at_ = now;
    ATLAS_LOG_WARNING(
        "CellAppMgr: HA snapshot file size at {}% of {} byte ceiling ({} bytes) — "
        "save will start rejecting writes once it crosses the ceiling",
        SnapshotSizeHighWaterPct(), kMaxSnapshotFileBytes, bytes.size());
  } else if (decision.should_reset) {
    last_snapshot_size_warning_at_ = {};
  }
  return {};
}

auto CellAppMgr::RestoreSnapshotFromFile(const std::filesystem::path& path) -> Result<void> {
  if (path.empty()) return {};
  last_snapshot_restore_attempt_at_ = Clock::now();
  last_snapshot_restore_topology_.clear();
  last_snapshot_restore_topology_pending_ack_ = 0;
  auto restore_one = [this](const std::filesystem::path& candidate) -> Result<void> {
    if (!fs::Exists(candidate)) {
      return Error{ErrorCode::kNotFound,
                   std::format("snapshot file not found: {}", candidate.string())};
    }
    auto size = fs::FileSize(candidate);
    if (!size) return size.Error();
    if (*size > kMaxSnapshotFileBytes) {
      return Error{ErrorCode::kInvalidArgument, "CellAppMgr snapshot: file too large"};
    }
    auto bytes = fs::ReadFile(candidate);
    if (!bytes) return bytes.Error();
    auto restore = Restore(std::span<const std::byte>(bytes->data(), bytes->size()));
    if (!restore) return restore;
    ++snapshot_restore_count_;
    last_snapshot_bytes_ = bytes->size();
    last_snapshot_restore_at_ = Clock::now();
    last_snapshot_restore_topology_ = BuildTopologyFingerprint();
    last_snapshot_restore_topology_pending_ack_ = TopologyPendingAckCount();
    return {};
  };

  auto primary = restore_one(path);
  if (primary) {
    last_snapshot_restore_source_ = "primary";
    last_snapshot_restore_path_ = path;
    last_snapshot_restore_error_.clear();
    last_snapshot_restore_primary_error_.clear();
    return {};
  }

  const auto backup_path = SnapshotBackupPath(path);
  auto backup = restore_one(backup_path);
  if (backup) {
    ++snapshot_fallback_restore_count_;
    last_snapshot_restore_source_ = "backup";
    last_snapshot_restore_path_ = backup_path;
    last_snapshot_restore_error_.clear();
    last_snapshot_restore_primary_error_ = primary.Error().Message();
    ATLAS_LOG_WARNING("CellAppMgr: restored HA snapshot backup {} after primary failed: {}",
                      backup_path.string(), primary.Error().Message());
    return {};
  }
  if (primary.Error().Code() == ErrorCode::kNotFound &&
      backup.Error().Code() == ErrorCode::kNotFound) {
    last_snapshot_restore_source_ = "none";
    last_snapshot_restore_path_.clear();
    last_snapshot_restore_topology_.clear();
    last_snapshot_restore_topology_pending_ack_ = 0;
    last_snapshot_restore_error_.clear();
    last_snapshot_restore_primary_error_.clear();
    return primary.Error();
  }
  ++snapshot_restore_failure_count_;
  ++snapshot_failure_count_;
  last_snapshot_restore_source_ = "none";
  last_snapshot_restore_path_.clear();
  last_snapshot_restore_topology_.clear();
  last_snapshot_restore_topology_pending_ack_ = 0;
  last_snapshot_restore_primary_error_ = primary.Error().Message();
  if (primary.Error().Code() == ErrorCode::kNotFound) {
    last_snapshot_restore_error_ = backup.Error().Message();
    return backup.Error();
  }
  if (backup.Error().Code() == ErrorCode::kNotFound) {
    last_snapshot_restore_error_ = primary.Error().Message();
    return primary.Error();
  }
  auto error = Error{primary.Error().Code(),
                     std::format("{}; backup failed: {}", primary.Error().Message(),
                                 backup.Error().Message())};
  last_snapshot_restore_error_ = error.Message();
  return error;
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
  wr.Add<uint64_t>("cellappmgr/ha/mgr_generation",
                   std::function<uint64_t()>([this] { return mgr_generation_; }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_path",
                      std::function<std::string()>(
                          [this] { return Config().snapshot_path.string(); }));
  wr.Add<int>("cellappmgr/ha/snapshot_interval_ms",
              std::function<int()>([this] { return Config().snapshot_interval_ms; }));
  wr.Add<std::size_t>("cellappmgr/ha/snapshot_bytes",
                      std::function<std::size_t()>(
                          [this] { return last_snapshot_bytes_; }));
  wr.Add<bool>("cellappmgr/ha/snapshot_file_present",
               std::function<bool()>(
                   [this] { return SnapshotFilePresentForWatcher(); }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_file_bytes",
                   std::function<uint64_t()>(
                       [this] { return SnapshotFileBytesForWatcher(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_file_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotFileStatusSummary(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_file_topology_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotFileTopologyStatusSummary(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_backup_path",
                      std::function<std::string()>(
                          [this] { return SnapshotBackupPathForWatcher(); }));
  wr.Add<bool>("cellappmgr/ha/snapshot_backup_present",
               std::function<bool()>(
                   [this] { return SnapshotBackupPresentForWatcher(); }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_backup_bytes",
                   std::function<uint64_t()>(
                       [this] { return SnapshotBackupBytesForWatcher(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_backup_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotBackupStatusSummary(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_backup_topology_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotBackupTopologyStatusSummary(); }));
  wr.Add<int64_t>("cellappmgr/ha/snapshot_last_save_attempt_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotAttemptAgeMsForWatcher(); }));
  wr.Add<int64_t>("cellappmgr/ha/snapshot_last_save_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotSaveAgeMsForWatcher(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_save_path",
                      std::function<std::string()>(
                          [this] { return last_snapshot_save_path_.string(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_save_topology",
                      std::function<std::string()>(
                          [this] { return last_snapshot_save_topology_; }));
  wr.Add<std::size_t>("cellappmgr/ha/snapshot_last_save_topology_pending_ack",
                      std::function<std::size_t()>(
                          [this] { return last_snapshot_save_topology_pending_ack_; }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_save_error",
                      std::function<std::string()>(
                          [this] { return last_snapshot_save_error_; }));
  wr.Add<bool>("cellappmgr/ha/snapshot_dirty",
               std::function<bool()>([this] { return snapshot_dirty_; }));
  wr.Add<int64_t>("cellappmgr/ha/snapshot_dirty_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotDirtyAgeMsForWatcher(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_dirty_reason",
                      std::function<std::string()>([this] {
                        return snapshot_dirty_ ? snapshot_dirty_reason_ : std::string{};
                      }));
  wr.Add<bool>("cellappmgr/ha/snapshot_save_stale",
               std::function<bool()>(
                   [this] { return SnapshotSaveStaleForWatcher(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotStatusSummary(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_restore_source",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_source_; }));
  wr.Add<int64_t>("cellappmgr/ha/snapshot_last_restore_attempt_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotRestoreAttemptAgeMsForWatcher(); }));
  wr.Add<int64_t>("cellappmgr/ha/snapshot_last_restore_age_ms",
                  std::function<int64_t()>(
                      [this] { return LastSnapshotRestoreAgeMsForWatcher(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_restore_path",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_path_.string(); }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_restore_topology",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_topology_; }));
  wr.Add<std::size_t>("cellappmgr/ha/snapshot_last_restore_topology_pending_ack",
                      std::function<std::size_t()>(
                          [this] { return last_snapshot_restore_topology_pending_ack_; }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_restore_error",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_error_; }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_last_restore_primary_error",
                      std::function<std::string()>(
                          [this] { return last_snapshot_restore_primary_error_; }));
  wr.Add<std::string>("cellappmgr/ha/snapshot_restore_status",
                      std::function<std::string()>(
                          [this] { return BuildSnapshotRestoreStatusSummary(); }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_saves",
                   std::function<uint64_t()>(
                       [this] { return snapshot_save_count_; }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_restores",
                   std::function<uint64_t()>(
                       [this] { return snapshot_restore_count_; }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_fallback_restores",
                   std::function<uint64_t()>(
                       [this] { return snapshot_fallback_restore_count_; }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_save_failures",
                   std::function<uint64_t()>(
                       [this] { return snapshot_save_failure_count_; }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_restore_failures",
                   std::function<uint64_t()>(
                       [this] { return snapshot_restore_failure_count_; }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_failures",
                   std::function<uint64_t()>(
                       [this] { return snapshot_failure_count_; }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_backup_skips",
                   std::function<uint64_t()>(
                       [this] { return snapshot_backup_skip_count_; }));
  wr.Add<uint64_t>("cellappmgr/ha/snapshot_max_bytes",
                   std::function<uint64_t()>(
                       [] { return kMaxSnapshotFileBytes; }));
  wr.Add<uint32_t>("cellappmgr/ha/snapshot_size_high_water_pct",
                   std::function<uint32_t()>(
                       [this] { return SnapshotSizeHighWaterPct(); }));
  wr.Add<std::size_t>("cellappmgr/ha/restored_cellapps",
                      std::function<std::size_t()>(
                          [this] { return RestoredCellAppCount(); }));
  wr.Add<std::size_t>("cellappmgr/ha/reattach_pending",
                      std::function<std::size_t()>(
                          [this] { return PendingReattachCellAppCount(); }));
  wr.Add<std::size_t>("cellappmgr/ha/reattach_completed_count",
                      std::function<std::size_t()>(
                          [this] { return CompletedReattachCellAppCount(); }));
  wr.Add<std::size_t>("cellappmgr/ha/reattach_stuck",
                      std::function<std::size_t()>(
                          [this] { return StuckReattachCellAppCount(); }));
  wr.Add<bool>("cellappmgr/ha/reattach_completed",
               std::function<bool()>([this] { return ReattachCompleted(); }));
  wr.Add<std::string>("cellappmgr/ha/reattach_state",
                      std::function<std::string()>(
                          [this] { return ReattachStateForWatcher(); }));
  wr.Add<std::string>("cellappmgr/ha/reattach_status",
                      std::function<std::string()>(
                          [this] { return BuildReattachStatusSummary(); }));
  wr.Add<bool>("cellappmgr/ha/restore_gate_active",
               std::function<bool()>([this] { return RestoreGateActiveForWatcher(); }));
  wr.Add<std::size_t>("cellappmgr/ha/restore_gate_blocked_pending_geometry",
                      std::function<std::size_t()>(
                          [this] { return PendingGeometryRestoreGateBlockedCount(); }));
  wr.Add<std::string>("cellappmgr/ha/restore_gate_status",
                      std::function<std::string()>(
                          [this] { return BuildRestoreGateStatusSummary(); }));
  wr.Add<uint64_t>("cellappmgr/ha/reattach_registry_audits",
                   std::function<uint64_t()>(
                       [this] { return reattach_registry_audit_count_; }));
  wr.Add<std::size_t>("cellappmgr/ha/reattach_registry_last_missing",
                      std::function<std::size_t()>(
                          [this] { return last_reattach_registry_missing_; }));
  wr.Add<std::size_t>("cellappmgr/ha/reattach_registry_last_blocked",
                      std::function<std::size_t()>(
                          [this] { return last_reattach_registry_blocked_; }));
  wr.Add<uint64_t>("cellappmgr/ha/reattach_registry_reconciled_total",
                   std::function<uint64_t()>(
                       [this] { return reattach_registry_reconciled_total_; }));
  wr.Add<std::string>("cellappmgr/ha/reattach_registry_status",
                      std::function<std::string()>(
                          [this] { return BuildReattachRegistryStatusSummary(); }));
}

void CellAppMgr::OnTickComplete() {
  ManagerApp::OnTickComplete();
  DrainPendingGeometryBroadcasts();
  AuditRetireDrainWatchdog();
  AuditReattachWatchdog();
  AuditReattachRegistry();
  DrainExpiredCreateSpaceRequests();
  const auto tick = GameTime();
  if (tick - last_balance_tick_ >= kBalanceTickInterval) {
    last_balance_tick_ = tick;
    TickLoadBalance();
  }
  if (!Config().snapshot_path.empty() && Config().snapshot_interval_ms > 0) {
    const auto now = Clock::now();
    const auto interval =
        std::chrono::duration_cast<Duration>(Milliseconds(Config().snapshot_interval_ms));
    const auto dirty_interval = std::min(interval, kDirtySnapshotFlushInterval);
    const bool never_attempted =
        last_snapshot_attempt_at_.time_since_epoch() == Duration::zero();
    const auto since_attempt = now - last_snapshot_attempt_at_;
    if (snapshot_dirty_ && (never_attempted || since_attempt >= dirty_interval)) {
      SaveConfiguredSnapshot("dirty");
    } else if (never_attempted || since_attempt >= interval) {
      SaveConfiguredSnapshot("periodic");
    }
  }
}

void CellAppMgr::MarkSnapshotDirty(const char* reason) {
  if (!snapshot_dirty_) snapshot_dirty_at_ = Clock::now();
  snapshot_dirty_ = true;
  snapshot_dirty_reason_ = reason == nullptr ? "unknown" : reason;
}

void CellAppMgr::SaveConfiguredSnapshot(const char* context) {
  if (Config().snapshot_path.empty()) return;
  auto save = SaveSnapshotToFile(Config().snapshot_path);
  if (save) {
    last_snapshot_save_warning_at_ = {};
    return;
  }
  // Bad path (disk full, permission, env corruption) repeats every tick at
  // snapshot_interval_ms; throttle the WARNING line so logs stay readable.
  const auto now = Clock::now();
  const auto throttle = std::chrono::duration_cast<Duration>(Milliseconds{5000});
  if (last_snapshot_save_warning_at_ != TimePoint{} &&
      now - last_snapshot_save_warning_at_ < throttle) {
    return;
  }
  last_snapshot_save_warning_at_ = now;
  ATLAS_LOG_WARNING("CellAppMgr: HA snapshot {} save failed: {}", context,
                    save.Error().Message());
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
    existing.needs_reattach = false;
    // restored_from_snapshot stays sticky until OnCellAppDeath so verify
    // scripts can assert restored_cellapps >= min_cellapps after a takeover.
    existing.last_reattach_watchdog_log_at = {};
    SendRegisterCellAppAck(ch, kInternalAddr, existing.app_id, /*success=*/true, "reattach");
    ReplayCellAppTopology(existing);
    SendRequestCellAppState(existing);
    ATLAS_LOG_INFO("CellAppMgr: CellApp reattached app_id={} internal={}:{}",
                   existing.app_id, kInternalAddr.Ip(), kInternalAddr.Port());
    return;
  }

  if (next_cellapp_app_id_ > kMaxCellAppAppId) {
    ATLAS_LOG_ERROR(
        "CellAppMgr: CellApp app_id pool exhausted (> {}) — rejecting register from {}:{}",
        kMaxCellAppAppId, kInternalAddr.Ip(), kInternalAddr.Port());
    SendRegisterCellAppAck(ch, kInternalAddr, /*app_id=*/0, /*success=*/false, "pool-exhausted");
    return;
  }

  const uint32_t app_id = next_cellapp_app_id_++;
  CellAppInfo info;
  info.internal_addr = kInternalAddr;
  info.app_id = app_id;
  info.channel = ch;
  info.registered_at = Clock::now();
  cellapps_.emplace(kInternalAddr, std::move(info));
  MarkSnapshotDirty("cellapp-register");

  SendRegisterCellAppAck(ch, kInternalAddr, app_id, /*success=*/true, "register");

  ATLAS_LOG_INFO("CellAppMgr: CellApp registered app_id={} internal={}:{}", app_id,
                 kInternalAddr.Ip(), kInternalAddr.Port());

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
  ack.mgr_generation = mgr_generation_;
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
  ack.snapshot_saves = snapshot_save_count_;
  ack.snapshot_failures = snapshot_failure_count_;
  ack.mgr_generation = mgr_generation_;
  ack.snapshot_dirty = snapshot_dirty_;
  ack.snapshot_save_stale = SnapshotSaveStaleForWatcher();
  (void)ch->SendMessage(ack);
}

void CellAppMgr::ReplayCellAppTopology(const CellAppInfo& info) {
  for (auto& [_, partition] : spaces_) {
    bool participates = false;
    for (const auto* leaf : partition.bsp.Leaves()) {
      if (leaf->cellapp_addr != info.internal_addr) continue;
      participates = true;
      const bool is_primary = leaf->cell_id == partition.bsp.PrimaryCellId();
      SendAddCell(info, partition.space_id, leaf->cell_id, leaf->bounds, is_primary,
                  partition.space_master_type);
    }
    if (participates && !HasPendingGeometryBroadcast(partition.space_id)) {
      SendGeometryToCellApp(info, partition);
    }
  }
}

void CellAppMgr::SendGeometryToCellApp(const CellAppInfo& target,
                                       const SpacePartition& partition) {
  if (target.channel == nullptr) return;
  BinaryWriter w;
  partition.bsp.Serialize(w);
  cellappmgr::UpdateGeometry msg;
  msg.space_id = partition.space_id;
  msg.geometry_version = partition.geometry_version;
  const auto blob = w.Detach();
  msg.bsp_blob.assign(blob.begin(), blob.end());
  msg.mgr_generation = mgr_generation_;
  if (auto r = target.channel->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("CellAppMgr: geometry replay failed space={} app_id={}: {}",
                      partition.space_id, target.app_id, r.Error().Message());
  }
}

void CellAppMgr::SendRequestCellAppState(const CellAppInfo& target) {
  if (target.channel == nullptr) return;
  cellappmgr::RequestCellAppState msg;
  if (auto r = target.channel->SendMessage(msg); !r) {
    ATLAS_LOG_WARNING("CellAppMgr: state request failed app_id={}: {}",
                      target.app_id, r.Error().Message());
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
    if (info.needs_reattach) {
      ATLAS_LOG_DEBUG("CellAppMgr: ignoring InformCellLoad for reattach-pending app_id={}",
                      msg.app_id);
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
  MarkSnapshotDirty("split-pending-geometry");
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
  if (PendingReattachCellAppCount() != 0) return;
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
  MarkSnapshotDirty("retire-handoff");
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
  if (app_it->second.needs_reattach) {
    ATLAS_LOG_DEBUG("CellAppMgr: ignoring AddCellToSpaceAck for reattach-pending app_id={}",
                    app_it->second.app_id);
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
  MarkSnapshotDirty("pending-geometry-ack");

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
    const auto pending_app = cellapps_.find(it->awaiting_addr);
    if (pending_app != cellapps_.end() && pending_app->second.needs_reattach) {
      ATLAS_LOG_WARNING(
          "CellAppMgr: AddCellToSpaceAck timeout space={} cell={} addr={}:{} - "
          "holding geometry until restored target reattaches",
          it->space_id, it->awaiting_cell_id, it->awaiting_addr.Ip(), it->awaiting_addr.Port());
      it->sent_at = now;
      ++it;
      continue;
    }
    if (!it->allow_timeout_broadcast) {
      ATLAS_LOG_WARNING(
          "CellAppMgr: AddCellToSpaceAck timeout space={} cell={} addr={}:{} - "
          "holding geometry until snapshot-gated primary handoff acks",
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
    MarkSnapshotDirty("pending-geometry-timeout");
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
    const bool changed = !drain.geometry_published;
    drain.geometry_published = true;
    drain.started_at = now;
    drain.last_progress_at = now;
    drain.last_watchdog_log_at = {};
    if (changed) MarkSnapshotDirty("retire-geometry-published");
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

auto CellAppMgr::IsReattachStuck(const CellAppInfo& info, TimePoint now) const -> bool {
  if (!info.restored_from_snapshot || !info.needs_reattach) return false;
  return now - info.registered_at >= ReattachWatchdogWindow();
}

void CellAppMgr::AuditReattachWatchdog() {
  if (cellapps_.empty()) return;
  const auto now = Clock::now();
  const auto watchdog_window = ReattachWatchdogWindow();
  const auto min_repeat_window = std::chrono::duration_cast<Duration>(Milliseconds{1000});
  const auto repeat_window =
      watchdog_window < min_repeat_window ? min_repeat_window : watchdog_window;
  for (auto& [_, info] : cellapps_) {
    if (!IsReattachStuck(info, now)) continue;
    if (info.last_reattach_watchdog_log_at != TimePoint{} &&
        now - info.last_reattach_watchdog_log_at < repeat_window) {
      continue;
    }
    ATLAS_LOG_WARNING("CellAppMgr: restored CellApp reattach stuck app_id={} addr={} age_ms={}",
                      info.app_id, info.internal_addr.ToString(),
                      DurationMs(now - info.registered_at));
    info.last_reattach_watchdog_log_at = now;
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
  MarkSnapshotDirty("cellapp-death");

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
  if (PendingReattachCellAppCount() != 0) return;
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
    if (app->needs_reattach) out += " reattach=1";
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

auto CellAppMgr::LastSnapshotAttemptAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_attempt_at_);
}

auto CellAppMgr::LastSnapshotSaveAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_save_at_);
}

auto CellAppMgr::LastSnapshotDirtyAgeMsForWatcher() const -> int64_t {
  return snapshot_dirty_ ? AgeMsSince(snapshot_dirty_at_) : -1;
}

auto CellAppMgr::LastSnapshotRestoreAttemptAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_restore_attempt_at_);
}

auto CellAppMgr::LastSnapshotRestoreAgeMsForWatcher() const -> int64_t {
  return AgeMsSince(last_snapshot_restore_at_);
}

auto CellAppMgr::SnapshotSaveStaleForWatcher() const -> bool {
  if (Config().snapshot_path.empty() || Config().snapshot_interval_ms <= 0) return false;
  const bool attempted = last_snapshot_attempt_at_.time_since_epoch() != Duration::zero();
  const bool saved = last_snapshot_save_at_.time_since_epoch() != Duration::zero();
  if (!saved) return attempted;
  return LastSnapshotSaveAgeMsForWatcher() >
         static_cast<int64_t>(Config().snapshot_interval_ms) * 2;
}

auto CellAppMgr::SnapshotSizeHighWaterPct() const -> uint32_t {
  if (last_snapshot_bytes_ == 0) return 0;
  const auto pct = (static_cast<uint64_t>(last_snapshot_bytes_) * 100u) / kMaxSnapshotFileBytes;
  return static_cast<uint32_t>(std::min<uint64_t>(pct, 100));
}

auto CellAppMgr::SnapshotFilePathForWatcher() const -> std::string {
  const auto& configured = Config().snapshot_path;
  const auto& base_path = configured.empty() ? last_snapshot_save_path_ : configured;
  return base_path.string();
}

auto CellAppMgr::SnapshotFilePresentForWatcher() const -> bool {
  return SnapshotFileReadinessForPath(SnapshotFilePathForWatcher(), false).present;
}

auto CellAppMgr::SnapshotFileBytesForWatcher() const -> uint64_t {
  return SnapshotFileReadinessForPath(SnapshotFilePathForWatcher(), false).bytes;
}

auto CellAppMgr::BuildSnapshotFileStatusSummary() const -> std::string {
  const auto path = SnapshotFilePathForWatcher();
  const auto readiness = SnapshotFileReadinessForPath(path, true);
  return std::format(
      "state={} path={} present={} bytes={} valid={} error_present={} error_detail={}",
      readiness.state, path, readiness.present ? 1 : 0, readiness.bytes,
      readiness.valid ? 1 : 0, readiness.error_present ? 1 : 0, readiness.error_detail);
}

auto CellAppMgr::BuildSnapshotFileTopologyStatusSummary() const -> std::string {
  return BuildSnapshotTopologyStatusSummary(SnapshotFilePathForWatcher(),
                                            last_snapshot_save_topology_);
}

auto CellAppMgr::SnapshotBackupPathForWatcher() const -> std::string {
  const auto& configured = Config().snapshot_path;
  const auto& base_path = configured.empty() ? last_snapshot_save_path_ : configured;
  if (base_path.empty()) return "";
  return SnapshotBackupPath(base_path).string();
}

auto CellAppMgr::SnapshotBackupPresentForWatcher() const -> bool {
  return SnapshotFileReadinessForPath(SnapshotBackupPathForWatcher(), false).present;
}

auto CellAppMgr::SnapshotBackupBytesForWatcher() const -> uint64_t {
  return SnapshotFileReadinessForPath(SnapshotBackupPathForWatcher(), false).bytes;
}

auto CellAppMgr::BuildSnapshotBackupStatusSummary() const -> std::string {
  const auto path = SnapshotBackupPathForWatcher();
  const auto readiness = SnapshotFileReadinessForPath(path, true);
  return std::format(
      "state={} path={} present={} bytes={} valid={} error_present={} error_detail={}",
      readiness.state, path, readiness.present ? 1 : 0, readiness.bytes,
      readiness.valid ? 1 : 0, readiness.error_present ? 1 : 0, readiness.error_detail);
}

auto CellAppMgr::BuildSnapshotBackupTopologyStatusSummary() const -> std::string {
  return BuildSnapshotTopologyStatusSummary(SnapshotBackupPathForWatcher(), "");
}

auto CellAppMgr::BuildSnapshotTopologyStatusSummary(const std::filesystem::path& path,
                                                    const std::string& expected_topology) const
    -> std::string {
  const auto path_text = path.string();
  if (path.empty()) {
    return std::format(
        "state=disabled path={} present=0 bytes=0 valid=0 restorable=0 topology_present=0 "
        "topology_pending_ack=0 matches_expected=0 error_present=0 error_detail=none",
        path_text);
  }
  if (!fs::Exists(path)) {
    return std::format(
        "state=missing path={} present=0 bytes=0 valid=0 restorable=0 topology_present=0 "
        "topology_pending_ack=0 matches_expected=0 error_present=0 error_detail=none",
        path_text);
  }
  auto size = fs::FileSize(path);
  if (!size) {
    return std::format(
        "state=unreadable path={} present=1 bytes=0 valid=0 restorable=0 "
        "topology_present=0 topology_pending_ack=0 matches_expected=0 error_present=1 "
        "error_detail={}",
        path_text, WatcherErrorDetail(size.Error().Message()));
  }
  const auto bytes_size = static_cast<uint64_t>(*size);
  if (bytes_size == 0) {
    return std::format(
        "state=empty path={} present=1 bytes=0 valid=0 restorable=0 topology_present=0 "
        "topology_pending_ack=0 matches_expected=0 error_present=0 error_detail=none",
        path_text);
  }
  if (*size > kMaxSnapshotFileBytes) {
    return std::format(
        "state=too_large path={} present=1 bytes={} valid=0 restorable=0 "
        "topology_present=0 topology_pending_ack=0 matches_expected=0 error_present=1 "
        "error_detail=file_too_large",
        path_text, bytes_size);
  }

  auto bytes = fs::ReadFile(path);
  if (!bytes) {
    return std::format(
        "state=unreadable path={} present=1 bytes={} valid=0 restorable=0 "
        "topology_present=0 topology_pending_ack=0 matches_expected=0 error_present=1 "
        "error_detail={}",
        path_text, bytes_size, WatcherErrorDetail(bytes.Error().Message()));
  }
  auto payload = SnapshotPayload(std::span<const std::byte>(bytes->data(), bytes->size()));
  if (!payload) {
    return std::format(
        "state=invalid path={} present=1 bytes={} valid=0 restorable=0 topology_present=0 "
        "topology_pending_ack=0 matches_expected=0 error_present=1 error_detail={}",
        path_text, bytes_size, WatcherErrorDetail(payload.Error().Message()));
  }

  EventDispatcher dispatcher{"cellappmgr-snapshot-topology"};
  NetworkInterface network{dispatcher};
  CellAppMgr verifier{dispatcher, network};
  auto restore = verifier.Restore(std::span<const std::byte>(bytes->data(), bytes->size()));
  if (!restore) {
    return std::format(
        "state=unrestorable path={} present=1 bytes={} valid=1 restorable=0 "
        "topology_present=0 topology_pending_ack=0 matches_expected=0 error_present=1 "
        "error_detail={}",
        path_text, bytes_size, WatcherErrorDetail(restore.Error().Message()));
  }

  const auto topology = verifier.BuildTopologyFingerprint();
  const auto pending_ack = verifier.TopologyPendingAckCount();
  const bool matches_expected = !expected_topology.empty() && topology == expected_topology;
  return std::format(
      "state=ready path={} present=1 bytes={} valid=1 restorable=1 topology_present=1 "
      "topology_pending_ack={} matches_expected={} error_present=0 error_detail=none",
      path_text, bytes_size, pending_ack, matches_expected ? 1 : 0);
}

auto CellAppMgr::BuildSnapshotStatusSummary() const -> std::string {
  const bool configured = !Config().snapshot_path.empty();
  const bool attempted = last_snapshot_attempt_at_.time_since_epoch() != Duration::zero();
  const bool saved = last_snapshot_save_at_.time_since_epoch() != Duration::zero();
  const bool stale = SnapshotSaveStaleForWatcher();

  const char* state = "disabled";
  if (configured) {
    if (stale) {
      state = "stale";
    } else if (!saved && !attempted) {
      state = "pending";
    } else if (!saved) {
      state = "failing";
    } else if (!last_snapshot_save_error_.empty()) {
      state = "degraded";
    } else if (snapshot_dirty_) {
      state = "dirty";
    } else {
      state = "healthy";
    }
  }
  const bool error_present = !last_snapshot_save_error_.empty();
  const auto error_detail =
      error_present ? WatcherErrorDetail(last_snapshot_save_error_) : std::string{"none"};
  const auto dirty_reason =
      snapshot_dirty_ ? WatcherErrorDetail(snapshot_dirty_reason_) : std::string{"none"};

  return std::format(
      "state={} configured={} interval_ms={} saves={} save_failures={} "
      "restore_failures={} failures={} stale={} last_attempt_age_ms={} "
      "last_save_age_ms={} last_restore_attempt_age_ms={} last_restore_age_ms={} "
      "bytes={} topology_present={} topology_pending_ack={} dirty={} dirty_age_ms={} "
      "dirty_reason={} error_present={} error_detail={}",
      state, configured ? 1 : 0, Config().snapshot_interval_ms, snapshot_save_count_,
      snapshot_save_failure_count_, snapshot_restore_failure_count_, snapshot_failure_count_,
      stale ? 1 : 0, LastSnapshotAttemptAgeMsForWatcher(), LastSnapshotSaveAgeMsForWatcher(),
      LastSnapshotRestoreAttemptAgeMsForWatcher(), LastSnapshotRestoreAgeMsForWatcher(),
      last_snapshot_bytes_, last_snapshot_save_topology_.empty() ? 0 : 1,
      last_snapshot_save_topology_pending_ack_, snapshot_dirty_ ? 1 : 0,
      LastSnapshotDirtyAgeMsForWatcher(), dirty_reason, error_present ? 1 : 0,
      error_detail);
}

auto CellAppMgr::BuildSnapshotRestoreStatusSummary() const -> std::string {
  const bool attempted =
      last_snapshot_restore_attempt_at_.time_since_epoch() != Duration::zero();
  const bool restored = last_snapshot_restore_at_.time_since_epoch() != Duration::zero();

  const char* state = "not_attempted";
  if (attempted) {
    if (!last_snapshot_restore_error_.empty()) {
      state = "failed";
    } else if (last_snapshot_restore_source_ == "primary") {
      state = "primary";
    } else if (last_snapshot_restore_source_ == "backup") {
      state = "fallback";
    } else {
      state = "missing";
    }
  }
  const bool error_present = !last_snapshot_restore_error_.empty();
  const bool primary_error_present = !last_snapshot_restore_primary_error_.empty();
  const auto error_detail =
      error_present ? WatcherErrorDetail(last_snapshot_restore_error_) : std::string{"none"};
  const auto primary_error_detail = primary_error_present
                                        ? WatcherErrorDetail(last_snapshot_restore_primary_error_)
                                        : std::string{"none"};

  return std::format(
      "state={} source={} restored={} restores={} fallback_restores={} "
      "restore_failures={} failures={} last_attempt_age_ms={} last_restore_age_ms={} "
      "error_present={} error_detail={} primary_error_present={} primary_error_detail={} "
      "topology_present={} topology_pending_ack={}",
      state, last_snapshot_restore_source_, restored ? 1 : 0, snapshot_restore_count_,
      snapshot_fallback_restore_count_, snapshot_restore_failure_count_, snapshot_failure_count_,
      LastSnapshotRestoreAttemptAgeMsForWatcher(), LastSnapshotRestoreAgeMsForWatcher(),
      error_present ? 1 : 0, error_detail, primary_error_present ? 1 : 0,
      primary_error_detail,
      last_snapshot_restore_topology_.empty() ? 0 : 1,
      last_snapshot_restore_topology_pending_ack_);
}

auto CellAppMgr::ReattachStateForWatcher() const -> std::string {
  if (RestoredCellAppCount() == 0) return "idle";
  if (StuckReattachCellAppCount() != 0) return "stuck";
  if (PendingReattachCellAppCount() == 0) return "complete";
  return "pending";
}

auto CellAppMgr::BuildReattachStatusSummary() const -> std::string {
  std::vector<const CellAppInfo*> apps;
  apps.reserve(cellapps_.size());
  for (const auto& [_, info] : cellapps_) {
    if (info.restored_from_snapshot) apps.push_back(&info);
  }
  std::sort(apps.begin(), apps.end(), [](const CellAppInfo* a, const CellAppInfo* b) {
    return a->app_id < b->app_id;
  });

  const auto now = Clock::now();
  const auto pending = PendingReattachCellAppCount();
  const auto stuck = StuckReattachCellAppCount();
  const auto completed_count = apps.size() - pending;
  const auto state = ReattachStateForWatcher();
  std::string out = std::format(
      "state={} restored={} pending={} completed={} stuck={} completed_count={}", state,
      apps.size(), pending, pending == 0 ? 1 : 0, stuck, completed_count);
  for (const auto* app : apps) {
    const bool app_stuck = IsReattachStuck(*app, now);
    const char* app_state =
        app->needs_reattach ? (app_stuck ? "stuck" : "pending") : "attached";
    out += std::format(" app={} addr={} state={}", app->app_id,
                       app->internal_addr.ToString(), app_state);
    if (app->needs_reattach) {
      out += std::format(" age_ms={}", DurationMs(now - app->registered_at));
    }
  }
  return out;
}

auto CellAppMgr::RestoreGateActiveForWatcher() const -> bool {
  return PendingReattachCellAppCount() != 0;
}

auto CellAppMgr::PendingGeometryRestoreGateBlockedCount() const -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(pending_geometry_broadcasts_.begin(), pending_geometry_broadcasts_.end(),
                    [this](const PendingGeometryBroadcast& pending) {
                      const auto app = cellapps_.find(pending.awaiting_addr);
                      return app != cellapps_.end() && app->second.needs_reattach;
                    }));
}

auto CellAppMgr::BuildRestoreGateStatusSummary() const -> std::string {
  const auto pending_reattach = PendingReattachCellAppCount();
  const auto blocked_pending_geometry = PendingGeometryRestoreGateBlockedCount();
  const bool lb_frozen = pending_reattach != 0;
  const bool active = lb_frozen || blocked_pending_geometry != 0;
  const char* state = active ? "closed" : "open";
  return std::format(
      "state={} active={} lb_frozen={} pending_reattach={} pending_geometry={} "
      "blocked_pending_geometry={}",
      state, active ? 1 : 0, lb_frozen ? 1 : 0, pending_reattach,
      pending_geometry_broadcasts_.size(), blocked_pending_geometry);
}

auto CellAppMgr::BuildReattachRegistryStatusSummary() const -> std::string {
  std::string state = "healthy";
  if (PendingReattachCellAppCount() == 0) {
    state = "idle";
  } else if (reattach_registry_audit_pending_) {
    state = "querying";
  } else if (!last_reattach_registry_error_.empty()) {
    state = "error";
  } else if (last_reattach_registry_blocked_ != 0) {
    state = "blocked";
  } else if (last_reattach_registry_reconciled_ != 0) {
    state = "reconciled";
  }
  const auto error_detail = last_reattach_registry_error_.empty()
                                ? std::string{"none"}
                                : WatcherErrorDetail(last_reattach_registry_error_);
  return std::format(
      "state={} audits={} query_pending={} pending_reattach={} last_missing={} "
      "last_blocked={} last_reconciled={} reconciled_total={} error_detail={}",
      state, reattach_registry_audit_count_, reattach_registry_audit_pending_ ? 1 : 0,
      PendingReattachCellAppCount(), last_reattach_registry_missing_,
      last_reattach_registry_blocked_, last_reattach_registry_reconciled_,
      reattach_registry_reconciled_total_, error_detail);
}

auto CellAppMgr::CellAppOwnsAnyLeaf(const Address& addr) const -> bool {
  for (const auto& [_, partition] : spaces_) {
    for (const auto* leaf : partition.bsp.Leaves()) {
      if (leaf->cellapp_addr == addr) return true;
    }
  }
  return false;
}

auto CellAppMgr::HasAssignableCellAppExcept(const Address& addr) const -> bool {
  const auto now = Clock::now();
  return std::any_of(cellapps_.begin(), cellapps_.end(), [&](const auto& entry) {
    return entry.first != addr && IsAssignableForLb(entry.second, now);
  });
}

void CellAppMgr::AuditReattachRegistry() {
  if (PendingReattachCellAppCount() == 0) {
    reattach_registry_audit_pending_ = false;
    last_reattach_registry_missing_ = 0;
    last_reattach_registry_blocked_ = 0;
    last_reattach_registry_reconciled_ = 0;
    last_reattach_registry_error_.clear();
    return;
  }
  if (reattach_registry_audit_pending_) return;
  const auto now = Clock::now();
  if (last_reattach_registry_audit_at_ != TimePoint{} &&
      now - last_reattach_registry_audit_at_ < kReattachRegistryAuditInterval) {
    return;
  }
  if (!GetMachinedClient().IsConnected()) {
    last_reattach_registry_error_ = "machined disconnected";
    return;
  }

  reattach_registry_audit_pending_ = true;
  last_reattach_registry_audit_at_ = now;
  ++reattach_registry_audit_count_;
  GetMachinedClient().QueryAsync(ProcessType::kCellApp,
                                 [this](std::vector<machined::ProcessInfo> infos) {
                                   OnReattachRegistryAudit(std::move(infos));
                                 });
}

void CellAppMgr::OnReattachRegistryAudit(std::vector<machined::ProcessInfo> infos) {
  reattach_registry_audit_pending_ = false;
  if (infos.empty() && PendingReattachCellAppCount() != 0) {
    last_reattach_registry_missing_ = 0;
    last_reattach_registry_blocked_ = 0;
    last_reattach_registry_reconciled_ = 0;
    last_reattach_registry_error_ = "cellapp registry query returned empty";
    return;
  }
  (void)ApplyReattachRegistryAudit(std::span<const machined::ProcessInfo>(infos.data(),
                                                                          infos.size()));
}

auto CellAppMgr::ApplyReattachRegistryAudit(std::span<const machined::ProcessInfo> infos)
    -> ReattachRegistryAuditResult {
  std::vector<Address> live_cellapps;
  live_cellapps.reserve(infos.size());
  for (const auto& info : infos) {
    if (info.process_type == ProcessType::kCellApp) live_cellapps.push_back(info.internal_addr);
  }

  std::vector<Address> missing;
  for (const auto& [addr, info] : cellapps_) {
    if (!info.restored_from_snapshot || !info.needs_reattach) continue;
    const auto found = std::find(live_cellapps.begin(), live_cellapps.end(), addr);
    if (found == live_cellapps.end()) missing.push_back(addr);
  }

  ReattachRegistryAuditResult result;
  result.missing = missing.size();
  last_reattach_registry_error_.clear();
  for (const auto& addr : missing) {
    if (!CellAppOwnsAnyLeaf(addr)) {
      cellapps_.erase(addr);
      MarkSnapshotDirty("reattach-registry-prune");
      ++result.reconciled;
      continue;
    }
    if (!HasAssignableCellAppExcept(addr)) {
      ++result.blocked;
      continue;
    }
    OnCellAppDeath(addr, /*reason=*/2);
    ++result.reconciled;
  }

  last_reattach_registry_missing_ = result.missing;
  last_reattach_registry_blocked_ = result.blocked;
  last_reattach_registry_reconciled_ = result.reconciled;
  reattach_registry_reconciled_total_ += result.reconciled;
  return result;
}

void CellAppMgr::ApplyReattachRegistryAuditForTest(
    std::span<const machined::ProcessInfo> infos) {
  (void)ApplyReattachRegistryAudit(infos);
}

void CellAppMgr::OnReattachRegistryAuditForTest(std::vector<machined::ProcessInfo> infos) {
  OnReattachRegistryAudit(std::move(infos));
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

auto CellAppMgr::RestoredCellAppCount() const -> std::size_t {
  return static_cast<std::size_t>(
      std::count_if(cellapps_.begin(), cellapps_.end(),
                    [](const auto& entry) { return entry.second.restored_from_snapshot; }));
}

auto CellAppMgr::PendingReattachCellAppCount() const -> std::size_t {
  return static_cast<std::size_t>(std::count_if(
      cellapps_.begin(), cellapps_.end(), [](const auto& entry) {
        return entry.second.restored_from_snapshot && entry.second.needs_reattach;
      }));
}

auto CellAppMgr::CompletedReattachCellAppCount() const -> std::size_t {
  return RestoredCellAppCount() - PendingReattachCellAppCount();
}

auto CellAppMgr::StuckReattachCellAppCount() const -> std::size_t {
  const auto now = Clock::now();
  return static_cast<std::size_t>(
      std::count_if(cellapps_.begin(), cellapps_.end(), [&](const auto& entry) {
        return IsReattachStuck(entry.second, now);
      }));
}

auto CellAppMgr::ReattachCompleted() const -> bool {
  return PendingReattachCellAppCount() == 0;
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
    bool changed = last_retire_app_id_ != 0;
    for (auto& [_, info] : cellapps_) {
      changed = changed || info.is_retiring;
      info.is_retiring = false;
    }
    last_retire_app_id_ = 0;
    if (changed) MarkSnapshotDirty("retire-clear");
    return true;
  }
  for (auto& [_, info] : cellapps_) {
    if (info.app_id != app_id) continue;
    const bool changed = !info.is_retiring || last_retire_app_id_ != app_id;
    info.is_retiring = true;
    last_retire_app_id_ = app_id;
    if (changed) MarkSnapshotDirty("retire-mark");
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
  MarkSnapshotDirty("retire-drain-complete");
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
  msg.mgr_generation = mgr_generation_;
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
  msg.mgr_generation = mgr_generation_;
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
    msg.mgr_generation = mgr_generation_;

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
        so.mgr_generation = mgr_generation_;
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
    MarkSnapshotDirty("geometry-broadcast");
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
