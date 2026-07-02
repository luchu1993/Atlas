#include "witness.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aoi_trigger.h"
#include "cell_entity.h"
#include "cellapp_config.h"
#include "foundation/clock.h"
#include "foundation/log.h"
#include "foundation/profiler.h"
#include "math/vector3.h"
#include "protocol/aoi_envelope.h"
#include "space.h"
#include "space/range_list.h"

namespace atlas {

namespace {

// Three-band LOD config snapshot, captured once per Update so config
// reloads can't mutate thresholds mid-tick. Close peers pump every
// interval[0] ticks bounded by max_peers[0]; same for medium/far.
struct LodConfig {
  double dist_sq[2];           // [close, medium]; far implied as "beyond"
  uint64_t interval[3];        // [close, medium, far]
  uint32_t max_peers[3];
  uint64_t starvation_threshold_ticks{0};

  static auto Snapshot() -> LodConfig {
    const float close_m = CellAppConfig::WitnessLodCloseDistanceM();
    const float medium_m = CellAppConfig::WitnessLodMediumDistanceM();
    return LodConfig{
        {static_cast<double>(close_m) * close_m, static_cast<double>(medium_m) * medium_m},
        {CellAppConfig::WitnessLodCloseIntervalTicks(),
         CellAppConfig::WitnessLodMediumIntervalTicks(),
         CellAppConfig::WitnessLodFarIntervalTicks()},
        {CellAppConfig::WitnessLodCloseMaxPeersPerTick(),
         CellAppConfig::WitnessLodMediumMaxPeersPerTick(),
         CellAppConfig::WitnessLodFarMaxPeersPerTick()},
        CellAppConfig::WitnessLodStarvationThresholdTicks()};
  }

  // Returns 0=close, 1=medium, 2=far.
  [[nodiscard]] auto BandIndex(double priority) const -> std::size_t {
    if (priority < dist_sq[0]) return 0;
    if (priority < dist_sq[1]) return 1;
    return 2;
  }
};

// Squared distance: ordering preserved without sqrt.
auto ComputePriority(const math::Vector3& observer, const math::Vector3& target) -> double {
  const double dx = observer.x - target.x;
  const double dy = observer.y - target.y;
  const double dz = observer.z - target.z;
  return dx * dx + dy * dy + dz * dz;
}

auto IsAllZeroDelta(std::span<const std::byte> delta) -> bool {
  return std::all_of(delta.begin(), delta.end(), [](std::byte b) { return b == std::byte{0}; });
}

// Process-wide monotonic clock; AvatarFilter consumes deltas, so absolute
// origin doesn't matter as long as readings within a single entity stream
// are monotonic.
auto MonotonicSeconds() -> double {
  using namespace std::chrono;
  return duration_cast<duration<double>>(Clock::now().time_since_epoch()).count();
}

// Wire: [u8 kind][u32 LE entity_id][payload bytes...].
template <std::size_t N>
auto MakeEnvelope(CellAoIEnvelopeKind kind, EntityID public_entity_id,
                  std::span<const std::byte> payload) -> std::vector<std::byte> {
  (void)N;
  std::vector<std::byte> out;
  out.reserve(1 + 4 + payload.size());
  out.push_back(static_cast<std::byte>(kind));
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((public_entity_id >> (i * 8)) & 0xFF));
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// Wire: [u8 kind][u32 LE entity_id][u64 LE event_seq][delta bytes].
auto BuildPropertyUpdateEnvelope(EntityID public_entity_id, uint64_t event_seq,
                                 std::span<const std::byte> delta) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(1 + 4 + sizeof(uint64_t) + delta.size());
  out.push_back(static_cast<std::byte>(CellAoIEnvelopeKind::kEntityPropertyUpdate));
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((public_entity_id >> (i * 8)) & 0xFF));
  }
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::byte>((event_seq >> (i * 8)) & 0xFF));
  }
  out.insert(out.end(), delta.begin(), delta.end());
  return out;
}

// Wire: [u8 kind][u32 LE id][u16 LE type][3f LE pos][3f LE dir]
//       [u8 on_ground][f64 LE server_time][peer_snapshot bytes].
auto BuildEnterEnvelope(EntityID public_entity_id, uint16_t type_id, const math::Vector3& pos,
                        const math::Vector3& dir, bool on_ground,
                        std::span<const std::byte> owner_snapshot) -> std::vector<std::byte> {
  constexpr std::size_t kHeaderBytes = 1 + sizeof(uint32_t);
  constexpr std::size_t kFixedPayload =
      sizeof(uint16_t) + 6 * sizeof(float) + sizeof(uint8_t) + sizeof(double);
  std::vector<std::byte> out;
  out.resize(kHeaderBytes + kFixedPayload + owner_snapshot.size());
  auto* p = out.data();
  *p++ = static_cast<std::byte>(CellAoIEnvelopeKind::kEntityEnter);
  for (int i = 0; i < 4; ++i) {
    *p++ = static_cast<std::byte>((public_entity_id >> (i * 8)) & 0xFF);
  }
  std::memcpy(p, &type_id, sizeof(type_id));
  p += sizeof(type_id);
  std::memcpy(p, &pos.x, sizeof(float) * 3);
  p += sizeof(float) * 3;
  std::memcpy(p, &dir.x, sizeof(float) * 3);
  p += sizeof(float) * 3;
  *p++ = static_cast<std::byte>(on_ground ? 1 : 0);
  const double server_time = MonotonicSeconds();
  std::memcpy(p, &server_time, sizeof(double));
  p += sizeof(double);
  if (!owner_snapshot.empty()) {
    std::memcpy(p, owner_snapshot.data(), owner_snapshot.size());
  }
  return out;
}

auto BuildSpaceDataInitEnvelope(SpaceID space_id, const SpaceData& data) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  // Two-pass: tally size for one reservation.
  std::size_t payload_bytes = sizeof(uint32_t);
  data.ForEach([&](SpaceData::KeyId, const SpaceData::ValueBytes& v) {
    payload_bytes += sizeof(uint16_t) + sizeof(uint32_t) + v.size();
  });
  out.reserve(1 + sizeof(uint32_t) + payload_bytes);
  out.push_back(static_cast<std::byte>(CellAoIEnvelopeKind::kSpaceDataInit));
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::byte>((space_id >> (i * 8)) & 0xFF));
  const auto count = static_cast<uint32_t>(data.Size());
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::byte>((count >> (i * 8)) & 0xFF));
  data.ForEach([&](SpaceData::KeyId key, const SpaceData::ValueBytes& v) {
    for (int i = 0; i < 2; ++i)
      out.push_back(static_cast<std::byte>((key >> (i * 8)) & 0xFF));
    const auto vlen = static_cast<uint32_t>(v.size());
    for (int i = 0; i < 4; ++i)
      out.push_back(static_cast<std::byte>((vlen >> (i * 8)) & 0xFF));
    for (uint8_t b : v) out.push_back(static_cast<std::byte>(b));
  });
  return out;
}

auto BuildSpaceDataUpdateEnvelope(SpaceID space_id, uint16_t key_id,
                                  std::span<const uint8_t> value) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(1 + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(uint32_t) + value.size());
  out.push_back(static_cast<std::byte>(CellAoIEnvelopeKind::kSpaceDataUpdate));
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::byte>((space_id >> (i * 8)) & 0xFF));
  for (int i = 0; i < 2; ++i)
    out.push_back(static_cast<std::byte>((key_id >> (i * 8)) & 0xFF));
  const auto vlen = static_cast<uint32_t>(value.size());
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::byte>((vlen >> (i * 8)) & 0xFF));
  for (uint8_t b : value) out.push_back(static_cast<std::byte>(b));
  return out;
}

auto BuildSpaceDataDeleteEnvelope(SpaceID space_id, uint16_t key_id) -> std::vector<std::byte> {
  std::vector<std::byte> out;
  out.reserve(1 + sizeof(uint32_t) + sizeof(uint16_t));
  out.push_back(static_cast<std::byte>(CellAoIEnvelopeKind::kSpaceDataDelete));
  for (int i = 0; i < 4; ++i)
    out.push_back(static_cast<std::byte>((space_id >> (i * 8)) & 0xFF));
  for (int i = 0; i < 2; ++i)
    out.push_back(static_cast<std::byte>((key_id >> (i * 8)) & 0xFF));
  return out;
}

}  // namespace

void Witness::SendSpaceDataInit(SpaceID space_id, const SpaceData& data) {
  if (!send_reliable_) return;
  auto env = BuildSpaceDataInitEnvelope(space_id, data);
  send_reliable_(env);
}

void Witness::SendSpaceDataUpdate(SpaceID space_id, uint16_t key_id,
                                  std::span<const uint8_t> value) {
  if (!send_reliable_) return;
  auto env = BuildSpaceDataUpdateEnvelope(space_id, key_id, value);
  send_reliable_(env);
}

void Witness::SendSpaceDataDelete(SpaceID space_id, uint16_t key_id) {
  if (!send_reliable_) return;
  auto env = BuildSpaceDataDeleteEnvelope(space_id, key_id);
  send_reliable_(env);
}

Witness::Witness(CellEntity& owner, float aoi_radius, float hysteresis, SendFn send_reliable,
                 SendFn send_unreliable)
    : owner_(owner),
      aoi_radius_(aoi_radius),
      hysteresis_(hysteresis),
      send_reliable_(std::move(send_reliable)),
      send_unreliable_(std::move(send_unreliable)) {}

Witness::~Witness() = default;

void Witness::Activate() {
  if (trigger_) return;
  trigger_ = std::make_unique<AoITrigger>(*this, owner_.RangeNode(), aoi_radius_,
                                          aoi_radius_ + hysteresis_);
  trigger_->Insert(owner_.GetSpace().GetRangeList());
}

void Witness::Deactivate(bool flush_leaves) {
  if (!trigger_) return;
  // Snapshot ids first — SendEntityLeave's callback may re-enter (channel
  // teardown, peer destruction in tests) and mutate aoi_map_ mid-iter.
  if (flush_leaves && send_reliable_) {
    std::vector<EntityID> ids;
    ids.reserve(aoi_map_.size());
    for (const auto& [peer_id, _] : aoi_map_) ids.push_back(peer_id);
    for (auto id : ids) (void)SendEntityLeave(id);
  }
  trigger_->Remove(owner_.GetSpace().GetRangeList());
  trigger_.reset();
  for (auto& [_, cache] : aoi_map_) {
    if (cache.entity) cache.entity->RemoveObserver(this);
  }
  aoi_map_.clear();
  band_scratch_.clear();
  pending_enter_ids_.clear();
  pending_gone_ids_.clear();
}

auto Witness::SerializeAoI() const -> std::vector<cellapp::WitnessAoIEntry> {
  std::vector<cellapp::WitnessAoIEntry> entries;
  entries.reserve(aoi_map_.size());
  // Only ship entity-owned seqs (absolute, replicated). lod_*_tick /
  // last_serviced_tick are this Witness's local tick_count_ — meaningless
  // on the destination, would freeze LOD if migrated.
  for (const auto& [peer_id, cache] : aoi_map_) {
    entries.push_back({peer_id, cache.last_event_seq, cache.last_volatile_seq});
  }
  return entries;
}

void Witness::InheritAoIFrom(const std::vector<cellapp::WitnessAoIEntry>& inherited) {
  if (inherited.empty()) return;
  std::unordered_map<EntityID, const cellapp::WitnessAoIEntry*> by_id;
  by_id.reserve(inherited.size());
  for (const auto& e : inherited) by_id.emplace(e.id, &e);

  // Overlap: client already mirrors the peer; clear pending Enter and
  // adopt inherited seqs so the next Update ships only the new frames.
  for (auto& [peer_id, cache] : aoi_map_) {
    auto it = by_id.find(peer_id);
    if (it == by_id.end()) continue;
    cache.flags &= ~EntityCache::kEnterPending;
    cache.last_event_seq = it->second->last_event_seq;
    cache.last_volatile_seq = it->second->last_volatile_seq;
    by_id.erase(it);
  }
  std::erase_if(pending_enter_ids_, [&aoi = aoi_map_](EntityID id) {
    auto it = aoi.find(id);
    return it != aoi.end() && (it->second.flags & EntityCache::kEnterPending) == 0;
  });

  // Orphans: client still thinks these are visible but they fell outside
  // the new AoI on handoff; emit Leave so the client drops them.
  if (!send_reliable_) return;
  for (const auto& [orphan_id, _] : by_id) (void)SendEntityLeave(orphan_id);
}

void Witness::SetAoIRadius(float new_radius, float new_hysteresis) {
  new_radius = std::max(0.1f, new_radius);
  const float max_radius = CellAppConfig::MaxAoIRadius();
  if (new_radius > max_radius) {
    ATLAS_LOG_WARNING("Witness::SetAoIRadius: clamping entity {}'s AoI radius ({}) to max ({})",
                      owner_.Id(), new_radius, max_radius);
    new_radius = max_radius;
  }
  aoi_radius_ = new_radius;
  hysteresis_ = std::max(0.f, new_hysteresis);
  if (trigger_) trigger_->SetBounds(aoi_radius_, aoi_radius_ + hysteresis_);
}

void Witness::HandleAoIEnter(CellEntity& peer) {
  if (&peer == &owner_) return;

  // ~CellEntity's FLT_MAX shuffle drags dying peers past inner.lower;
  // reject so the cache can't latch onto an about-to-be-freed pointer.
  if (peer.IsDestroyed()) return;

  auto [it, inserted] = aoi_map_.try_emplace(peer.Id());
  auto& cache = it->second;

  // Hysteresis re-cross while still in AoI: client already sees the
  // peer, re-emitting a snapshot would be wasteful.
  if (!inserted && (cache.flags & EntityCache::kGone) == 0) return;

  cache.entity = &peer;
  cache.flags = EntityCache::kEnterPending;
  cache.last_serviced_tick = tick_count_;
  pending_enter_ids_.push_back(peer.Id());
  peer.AddObserver(this);
  UpdatePriority(cache);
}

void Witness::ForceOuterInsidePeer(RangeListNode& peer) {
  if (trigger_) trigger_->ForceOuterInsidePeer(peer);
}

void Witness::OnOwnerMoved(float old_x, float old_z) {
  if (!trigger_) return;
  trigger_->OnCentralMoved(old_x, old_z);
}

void Witness::HandleAoILeave(CellEntity& peer) {
  auto it = aoi_map_.find(peer.Id());
  if (it == aoi_map_.end()) return;
  // Mark kGone, drain at next Update - keeps Update's iteration stable
  // when a leave fires mid-tick from a trigger shuffle.
  it->second.flags |= EntityCache::kGone;
  it->second.flags &= ~EntityCache::kEnterPending;
  pending_gone_ids_.push_back(peer.Id());
  it->second.entity = nullptr;
  peer.RemoveObserver(this);
}

void Witness::UpdatePriority(EntityCache& cache) const {
  cache.priority = ComputePriority(owner_.Position(), cache.entity->Position());
}

auto Witness::SendEntityEnter(EntityCache& cache) -> Witness::UpdateStats {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityEnter");
  // send_reliable_ may re-entrantly destroy the peer; pin a local
  // pointer so the post-send seq capture stays consistent.
  CellEntity* const entity = cache.entity;

  std::span<const std::byte> enter_snapshot{};
  uint64_t pre_event_seq = 0;
  uint64_t pre_volatile_seq = 0;
  if (const auto* state = entity->GetReplicationState()) {
    enter_snapshot = std::span<const std::byte>(state->other_snapshot);
    pre_event_seq = state->latest_event_seq;
    pre_volatile_seq = state->latest_volatile_seq;
  }

  auto envelope = BuildEnterEnvelope(entity->Id(), entity->TypeId(), entity->Position(),
                                     entity->Direction(), entity->OnGround(), enter_snapshot);
  if (send_reliable_) send_reliable_(envelope);

  // Skip the seq stamp if HandleAoILeave yanked cache.entity during send.
  if (cache.entity == entity) {
    cache.last_event_seq = pre_event_seq;
    cache.last_volatile_seq = pre_volatile_seq;
  }
  return UpdateStats{.reliable_bytes = static_cast<uint64_t>(envelope.size())};
}

auto Witness::SendEntityLeave(EntityID peer_id) -> Witness::UpdateStats {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityLeave");
  auto envelope = MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityLeave, peer_id, {});
  if (send_reliable_) send_reliable_(envelope);
  return UpdateStats{.reliable_bytes = static_cast<uint64_t>(envelope.size())};
}

auto Witness::Update(uint32_t max_packet_bytes) -> Witness::UpdateStats {
  ATLAS_PROFILE_ZONE_N("Witness::Update");
  last_update_stats_ = {};
  if (!trigger_) return last_update_stats_;

  ++tick_count_;

  const LodConfig lod = LodConfig::Snapshot();

  UpdateStats stats;
  int64_t bytes_sent = 0;
  auto bill = [&](const UpdateStats& delta) {
    stats.reliable_bytes += delta.reliable_bytes;
    stats.unreliable_bytes += delta.unreliable_bytes;
    const auto room = std::numeric_limits<int64_t>::max() - bytes_sent;
    bytes_sent +=
        static_cast<int64_t>(std::min<uint64_t>(delta.TotalBytes(), static_cast<uint64_t>(room)));
  };
  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::Transitions");
    (void)bandwidth_deficit_;

    // Cap Enter bytes/count so a fresh observer joining a dense scene doesn't
    // burst past the cold-start reliable-UDP cwnd and condemn the channel.
    const uint32_t enter_byte_budget = CellAppConfig::WitnessEnterBytesPerTick();
    const uint32_t enter_count_budget = CellAppConfig::WitnessMaxEntersPerTick();

    // Closest-first: the entities the player can actually see arrive earliest.
    const math::Vector3 obs_pos = owner_.Position();
    std::sort(pending_enter_ids_.begin(), pending_enter_ids_.end(),
              [&](EntityID a, EntityID b) {
                auto dist_sq = [&](EntityID id) -> double {
                  auto* e = owner_.GetSpace().FindEntity(id);
                  if (e == nullptr) return std::numeric_limits<double>::max();
                  const double dx = static_cast<double>(e->Position().x) - obs_pos.x;
                  const double dz = static_cast<double>(e->Position().z) - obs_pos.z;
                  return dx * dx + dz * dz;
                };
                return dist_sq(a) < dist_sq(b);
              });

    uint32_t enter_bytes = 0;
    uint32_t enter_count = 0;
    std::vector<EntityID> deferred;
    deferred.reserve(pending_enter_ids_.size());

    for (std::size_t enter_idx = 0; enter_idx < pending_enter_ids_.size(); ++enter_idx) {
      const EntityID peer_id = pending_enter_ids_[enter_idx];
      auto it = aoi_map_.find(peer_id);
      if (it == aoi_map_.end()) continue;
      auto& cache = it->second;
      // Re-entrant destruction during a previous iteration may have
      // cleared kEnterPending and nulled cache.entity.
      if (!(cache.flags & EntityCache::kEnterPending) || !cache.entity) continue;
      // Some destruction paths free the peer without firing
      // HandleAoILeave; cross-check against the entity map and patch
      // the cache so the gone-list loop won't emit a stale Leave.
      CellEntity* live = owner_.GetSpace().FindEntity(peer_id);
      if (live != cache.entity) {
        const std::size_t observer_obs_count =
            owner_.GetWitness() ? owner_.GetWitness()->AoIMap().size() : 0;
        ATLAS_LOG_WARNING(
            "Witness: stale enter-pending cache "
            "observer={} peer_id={} cached={:p} live={:p} "
            "flags=0x{:02x} aoi_map_size={} space_id={}",
            owner_.Id(), static_cast<uint64_t>(peer_id), static_cast<const void*>(cache.entity),
            static_cast<const void*>(live), cache.flags, observer_obs_count,
            owner_.GetSpace().Id());
        aoi_map_.erase(it);
        continue;
      }
      // Always allow the first Enter through so a single oversized envelope
      // never deadlocks; cap subsequent ones by budget and count.
      if (enter_count > 0 &&
          (enter_bytes >= enter_byte_budget || enter_count >= enter_count_budget)) {
        deferred.push_back(peer_id);
        continue;
      }
      const auto sent = SendEntityEnter(cache);
      bill(sent);
      enter_bytes = static_cast<uint32_t>(std::min<uint64_t>(
          std::numeric_limits<uint32_t>::max(),
          static_cast<uint64_t>(enter_bytes) + sent.TotalBytes()));
      ++enter_count;
      cache.flags &= ~EntityCache::kEnterPending;
      // Far interval is the longest band; phase mod it covers every band.
      const uint64_t far_interval = std::max<uint64_t>(1, lod.interval[2]);
      cache.lod_enter_phase = enter_idx % far_interval;
    }

    // SendEntityLeave can re-entrantly destroy a peer, whose HandleAoILeave
    // push_backs here and reallocates mid-loop; drain a snapshot and let any
    // re-entrant ids fall to the next tick.
    std::vector<EntityID> gone = std::move(pending_gone_ids_);
    pending_gone_ids_.clear();
    for (auto id : gone) {
      auto it = aoi_map_.find(id);
      if (it == aoi_map_.end()) continue;
      // Same id may appear twice (left -> re-entered -> left); after the
      // first Leave the cache is erased so subsequent finds return end.
      if (!(it->second.flags & EntityCache::kGone)) continue;
      bill(SendEntityLeave(it->first));
      aoi_map_.erase(it);
    }

    pending_enter_ids_ = std::move(deferred);
  }

  // Per-band pump: collect eligible peers into a band-scoped scratch,
  // rank-cut to that band's cap, sort by distance, send updates. Each
  // band gets its own quota so a flood of far peers can't starve close
  // ones (BigWorld semantics).
  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::PerBandPump");
    const int64_t tick_budget = static_cast<int64_t>(max_packet_bytes) - bandwidth_deficit_;

    const bool starvation_enabled = lod.starvation_threshold_ticks > 0;
    for (std::size_t band = 0; band < 3; ++band) {
      if (lod.max_peers[band] == 0) continue;

      band_scratch_.clear();
      for (auto& [id, cache] : aoi_map_) {
        if (!cache.IsUpdatable()) continue;
        if (tick_count_ < cache.lod_next_update_tick) continue;
        UpdatePriority(cache);
        if (lod.BandIndex(cache.priority) != band) continue;
        // Promote starved peers above the rank cut so a band that's
        // permanently over-cap still drains every observer eventually.
        const double effective =
            starvation_enabled &&
                    tick_count_ - cache.last_serviced_tick > lod.starvation_threshold_ticks
                ? -1.0
                : cache.priority;
        band_scratch_.emplace_back(effective, id);
      }

      // Rank cut: keep the closest max_peers within this band (starved
      // peers carry priority=-1.0 so they survive the cut).
      const std::size_t cap = lod.max_peers[band];
      if (band_scratch_.size() > cap) {
        std::nth_element(band_scratch_.begin(), band_scratch_.begin() + cap, band_scratch_.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        band_scratch_.resize(cap);
      }
      std::sort(band_scratch_.begin(), band_scratch_.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });

      const uint64_t interval = std::max<uint64_t>(1, lod.interval[band]);
      for (const auto& [prio, id] : band_scratch_) {
        if (bytes_sent >= tick_budget) break;
        auto it = aoi_map_.find(id);
        if (it == aoi_map_.end()) continue;
        auto& cache = it->second;
        if (!cache.IsUpdatable()) continue;

        bill(SendEntityUpdate(cache));
        cache.last_serviced_tick = tick_count_;
        // lod_enter_phase offsets the first window only (set at AoI-enter,
        // cleared here) to stagger simultaneous entries.
        cache.lod_next_update_tick = tick_count_ + interval + (cache.lod_enter_phase % interval);
        cache.lod_enter_phase = 0;
      }
    }
  }

  const int64_t deficit = std::max<int64_t>(0, bytes_sent - max_packet_bytes);
  bandwidth_deficit_ =
      static_cast<int>(std::min<int64_t>(deficit, std::numeric_limits<int>::max()));

  // Deficit > one tick's budget signals structural overload; rate-limit
  // so a sustained issue logs once per ~10 s instead of every tick.
  if (bandwidth_deficit_ > static_cast<int>(max_packet_bytes)) {
    if (deficit_warn_counter_ == 0) {
      ATLAS_LOG_WARNING(
          "Witness[{}]: bandwidth deficit {}B > per-observer budget {}B; peer catch-up "
          "will lag. Consider increasing cellapp/witness_per_peer_bytes or "
          "cellapp/witness_max_per_observer_budget_bytes.",
          owner_.Id(), bandwidth_deficit_, max_packet_bytes);
    }
    if (++deficit_warn_counter_ >= kDeficitWarnEveryNTicks) deficit_warn_counter_ = 0;
  } else {
    deficit_warn_counter_ = 0;
  }
  last_update_stats_ = stats;
  return last_update_stats_;
}

auto Witness::SendEntityUpdate(EntityCache& cache) -> Witness::UpdateStats {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityUpdate");
  const auto* state = cache.entity->GetReplicationState();
  if (!state) return {};

  UpdateStats stats;

  if (state->latest_volatile_seq > cache.last_volatile_seq) {
    // EntityPositionUpdate built into a stack buffer (38 B). Wire:
    // [u8 kind][u32 id][3f pos][3f dir][u8 on_ground][f64 server_time].
    constexpr std::size_t kEnvelopeSize =
        1 + sizeof(uint32_t) + 6 * sizeof(float) + 1 + sizeof(double);
    std::array<std::byte, kEnvelopeSize> envelope_buf;
    std::size_t envelope_size = 0;
    {
      ATLAS_PROFILE_ZONE_N("Witness::Vol::Build");
      const auto& pos = cache.entity->Position();
      const auto& dir = cache.entity->Direction();
      const uint8_t og = cache.entity->OnGround() ? 1 : 0;
      const EntityID public_eid = cache.entity->Id();
      const double server_time = MonotonicSeconds();

      auto* p = envelope_buf.data();
      *p++ = static_cast<std::byte>(CellAoIEnvelopeKind::kEntityPositionUpdate);
      std::memcpy(p, &public_eid, sizeof(public_eid));
      p += sizeof(public_eid);
      std::memcpy(p, &pos.x, sizeof(float) * 3);
      p += sizeof(float) * 3;
      std::memcpy(p, &dir.x, sizeof(float) * 3);
      p += sizeof(float) * 3;
      *p++ = static_cast<std::byte>(og);
      std::memcpy(p, &server_time, sizeof(double));
      p += sizeof(double);
      envelope_size = static_cast<std::size_t>(p - envelope_buf.data());
    }
    std::span<const std::byte> envelope(envelope_buf.data(), envelope_size);
    {
      ATLAS_PROFILE_ZONE_N("Witness::Vol::Send");
      // Volatile prefers unreliable; fall back to reliable when tests
      // leave send_unreliable_ unset.
      if (send_unreliable_) {
        send_unreliable_(envelope);
        stats.unreliable_bytes += envelope.size();
      } else if (send_reliable_) {
        send_reliable_(envelope);
        stats.reliable_bytes += envelope.size();
      }
    }
    cache.last_volatile_seq = state->latest_volatile_seq;
  }

  if (state->latest_event_seq <= cache.last_event_seq) return stats;

  // history seqs are consecutive (PublishReplicationFrame pushes one
  // per call), so coverage = oldest frame's seq <= first_needed.
  const uint64_t first_needed = cache.last_event_seq + 1;
  const bool have_continuous_coverage =
      !state->history.empty() && state->history.front().event_seq <= first_needed;

  // Witness always serves the other-audience scope - HandleAoIEnter
  // excludes &peer == &owner_, so owner-scope replication never flows
  // through here. The owner client receives its own deltas via the
  // CellAppNativeProvider direct path.
  if (have_continuous_coverage) {
    for (const auto& frame : state->history) {
      if (frame.event_seq < first_needed) continue;
      if (frame.event_seq > state->latest_event_seq) break;

      const auto& delta_bytes = frame.other_delta;
      // Skip empty / all-zero deltas - seq still advances so the next
      // non-empty frame doesn't look like a gap on the client.
      if (!delta_bytes.empty() && !IsAllZeroDelta(delta_bytes)) {
        // Reuse the cached envelope across all witnesses watching this
        // peer this tick.
        auto& cached = frame.cached_other_envelope;
        if (cached.empty()) {
          ATLAS_PROFILE_ZONE_N("Witness::Event::Build");
          cached = BuildPropertyUpdateEnvelope(cache.entity->Id(), frame.event_seq, delta_bytes);
        }
        {
          ATLAS_PROFILE_ZONE_N("Witness::Event::Send");
          if (send_reliable_) send_reliable_(cached);
        }
        stats.reliable_bytes += cached.size();
      }
      cache.last_event_seq = frame.event_seq;
    }
  } else {
    ATLAS_PROFILE_ZONE_N("Witness::Snapshot");
    // Snapshot fallback - observer fell out of the history window.
    // Carry latest_event_seq so the next delta seq+1 doesn't gap-warn.
    auto envelope = BuildPropertyUpdateEnvelope(cache.entity->Id(), state->latest_event_seq,
                                                state->other_snapshot);
    if (send_reliable_) send_reliable_(envelope);
    stats.reliable_bytes += envelope.size();
    cache.last_event_seq = state->latest_event_seq;
  }
  return stats;
}

}  // namespace atlas
