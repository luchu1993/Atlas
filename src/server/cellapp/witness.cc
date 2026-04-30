#include "witness.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <utility>

#include "aoi_trigger.h"
#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "cellapp_config.h"
#include "foundation/log.h"
#include "foundation/profiler.h"
#include "math/vector3.h"
#include "space.h"
#include "space/range_list.h"

namespace atlas {

namespace {

// LOD bands: close < 25 m every tick, medium < 100 m every 3, far
// ≥ 100 m every 6 — fits the 8-frame history window at 10 Hz.
static constexpr double kLodCloseSq = 25.0 * 25.0;
static constexpr double kLodMediumSq = 100.0 * 100.0;
static constexpr uint64_t kLodCloseInterval = 1;
static constexpr uint64_t kLodMediumInterval = 3;
static constexpr uint64_t kLodFarInterval = 6;

// Squared distance: make_heap only cares about ordering and a² < b²
// iff a < b for non-negative magnitudes, so we skip the sqrt.
auto ComputePriority(const math::Vector3& observer, const math::Vector3& target) -> double {
  const double dx = observer.x - target.x;
  const double dy = observer.y - target.y;
  const double dz = observer.z - target.z;
  return dx * dx + dy * dy + dz * dz;
}

auto IsAllZeroDelta(std::span<const std::byte> delta) -> bool {
  return std::all_of(delta.begin(), delta.end(), [](std::byte b) { return b == std::byte{0}; });
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
//       [u8 on_ground][peer_snapshot bytes].
auto BuildEnterEnvelope(EntityID public_entity_id, uint16_t type_id, const math::Vector3& pos,
                        const math::Vector3& dir, bool on_ground,
                        std::span<const std::byte> owner_snapshot) -> std::vector<std::byte> {
  constexpr std::size_t kHeaderBytes = 1 + sizeof(uint32_t);
  constexpr std::size_t kFixedPayload = sizeof(uint16_t) + 6 * sizeof(float) + sizeof(uint8_t);
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
  if (!owner_snapshot.empty()) {
    std::memcpy(p, owner_snapshot.data(), owner_snapshot.size());
  }
  return out;
}

}  // namespace

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

void Witness::Deactivate() {
  if (!trigger_) return;
  trigger_->Remove(owner_.GetSpace().GetRangeList());
  trigger_.reset();
  for (auto& [_, cache] : aoi_map_) {
    if (cache.entity) cache.entity->RemoveObserver(this);
  }
  aoi_map_.clear();
  priority_queue_.clear();
  pending_enter_ids_.clear();
  pending_gone_ids_.clear();
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
  // Mark kGone, drain at next Update — keeps Update's iteration stable
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

auto Witness::SendEntityEnter(EntityCache& cache) -> std::size_t {
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
  if (send_reliable_) send_reliable_(owner_.Id(), envelope);

  // Skip the seq stamp if HandleAoILeave yanked cache.entity during send.
  if (cache.entity == entity) {
    cache.last_event_seq = pre_event_seq;
    cache.last_volatile_seq = pre_volatile_seq;
  }
  return envelope.size();
}

auto Witness::SendEntityLeave(EntityID peer_id) -> std::size_t {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityLeave");
  auto envelope = MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityLeave, peer_id, {});
  if (send_reliable_) send_reliable_(owner_.Id(), envelope);
  return envelope.size();
}

auto Witness::LodIntervalForDistSq(double dist_sq) -> uint64_t {
  if (dist_sq < kLodCloseSq) return kLodCloseInterval;
  if (dist_sq < kLodMediumSq) return kLodMediumInterval;
  return kLodFarInterval;
}

void Witness::Update(uint32_t max_packet_bytes) {
  ATLAS_PROFILE_ZONE_N("Witness::Update");
  if (!trigger_) return;

  ++tick_count_;

  int bytes_sent = 0;
  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::Transitions");

    // Enters/Leaves bypass the byte budget — dropping them would
    // deadlock the aoi_map_ state machine.
    (void)bandwidth_deficit_;

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
      bytes_sent += static_cast<int>(SendEntityEnter(cache));
      cache.flags &= ~EntityCache::kEnterPending;
      cache.lod_enter_phase = enter_idx % kLodFarInterval;
    }

    for (auto id : pending_gone_ids_) {
      auto it = aoi_map_.find(id);
      if (it == aoi_map_.end()) continue;
      // Same id may appear twice (left → re-entered → left); after the
      // first Leave the cache is erased so subsequent finds return end.
      if (!(it->second.flags & EntityCache::kGone)) continue;
      // Map key carries the unified entity id — no need to cache it on
      // the cache entry: peer.Id() == peer.Id() since phase 2.
      bytes_sent += static_cast<int>(SendEntityLeave(it->first));
      aoi_map_.erase(it);
    }

    pending_enter_ids_.clear();
    pending_gone_ids_.clear();
  }

  // Rebuild the priority heap each tick — observer position changes
  // make priorities stale anyway. LOD gate filters scheduled peers.
  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::PriorityHeap");
    priority_queue_.clear();
    priority_queue_.reserve(aoi_map_.size());
    for (auto& [id, cache] : aoi_map_) {
      if (!cache.IsUpdatable()) continue;
      if (tick_count_ < cache.lod_next_update_tick) continue;
      UpdatePriority(cache);
      priority_queue_.emplace_back(cache.priority, id);
    }
    std::make_heap(priority_queue_.begin(), priority_queue_.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  }

  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::Pump");
    const int tick_budget = static_cast<int>(max_packet_bytes) - bandwidth_deficit_;
    // Cap peers/tick to bound serialisation CPU; RW config so ops can
    // retune live without rebuild.
    const std::size_t max_peers = CellAppConfig::WitnessMaxPeersPerTick();
    std::size_t peers_updated = 0;
    while (!priority_queue_.empty() && bytes_sent < tick_budget && peers_updated < max_peers) {
      std::pop_heap(priority_queue_.begin(), priority_queue_.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
      const auto [prio, id] = priority_queue_.back();
      priority_queue_.pop_back();

      auto it = aoi_map_.find(id);
      if (it == aoi_map_.end()) continue;
      auto& cache = it->second;
      if (!cache.IsUpdatable()) continue;

      bytes_sent += static_cast<int>(SendEntityUpdate(cache));
      ++peers_updated;
      // lod_enter_phase offsets the first window only (set at AoI-enter,
      // cleared here) to stagger simultaneous entries.
      const uint64_t interval = LodIntervalForDistSq(cache.priority);
      cache.lod_next_update_tick = tick_count_ + interval + (cache.lod_enter_phase % interval);
      cache.lod_enter_phase = 0;
    }
  }

  bandwidth_deficit_ = std::max(0, bytes_sent - static_cast<int>(max_packet_bytes));

  // Deficit > one tick's budget signals structural overload; rate-limit
  // so a sustained issue logs once per ~10 s instead of every tick.
  if (bandwidth_deficit_ > static_cast<int>(max_packet_bytes)) {
    if (deficit_warn_counter_ == 0) {
      ATLAS_LOG_WARNING(
          "Witness[{}]: bandwidth deficit {}B > per-observer budget {}B; peer catch-up "
          "will lag. Consider increasing cellapp/witness_per_observer_budget_bytes.",
          owner_.Id(), bandwidth_deficit_, max_packet_bytes);
    }
    if (++deficit_warn_counter_ >= kDeficitWarnEveryNTicks) deficit_warn_counter_ = 0;
  } else {
    deficit_warn_counter_ = 0;
  }
}

auto Witness::SendEntityUpdate(EntityCache& cache) -> std::size_t {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityUpdate");
  const auto* state = cache.entity->GetReplicationState();
  if (!state) return 0;

  std::size_t bytes = 0;

  if (state->latest_volatile_seq > cache.last_volatile_seq) {
    // EntityPositionUpdate built into a stack buffer (30 B). Wire:
    // [u8 kind][u32 id][3f pos][3f dir][u8 on_ground].
    constexpr std::size_t kEnvelopeSize = 1 + sizeof(uint32_t) + 6 * sizeof(float) + 1;
    std::array<std::byte, kEnvelopeSize> envelope_buf;
    std::size_t envelope_size = 0;
    {
      ATLAS_PROFILE_ZONE_N("Witness::Vol::Build");
      const auto& pos = cache.entity->Position();
      const auto& dir = cache.entity->Direction();
      const uint8_t og = cache.entity->OnGround() ? 1 : 0;
      const EntityID public_eid = cache.entity->Id();

      auto* p = envelope_buf.data();
      *p++ = static_cast<std::byte>(CellAoIEnvelopeKind::kEntityPositionUpdate);
      std::memcpy(p, &public_eid, sizeof(public_eid));
      p += sizeof(public_eid);
      std::memcpy(p, &pos.x, sizeof(float) * 3);
      p += sizeof(float) * 3;
      std::memcpy(p, &dir.x, sizeof(float) * 3);
      p += sizeof(float) * 3;
      *p++ = static_cast<std::byte>(og);
      envelope_size = static_cast<std::size_t>(p - envelope_buf.data());
    }
    std::span<const std::byte> envelope(envelope_buf.data(), envelope_size);
    {
      ATLAS_PROFILE_ZONE_N("Witness::Vol::Send");
      // Volatile prefers unreliable; fall back to reliable when tests
      // leave send_unreliable_ unset.
      if (send_unreliable_) {
        send_unreliable_(owner_.Id(), envelope);
      } else if (send_reliable_) {
        send_reliable_(owner_.Id(), envelope);
      }
    }
    bytes += envelope.size();
    cache.last_volatile_seq = state->latest_volatile_seq;
  }

  if (state->latest_event_seq <= cache.last_event_seq) return bytes;

  // history seqs are consecutive (PublishReplicationFrame pushes one
  // per call), so coverage = oldest frame's seq ≤ first_needed.
  const uint64_t first_needed = cache.last_event_seq + 1;
  const bool have_continuous_coverage =
      !state->history.empty() && state->history.front().event_seq <= first_needed;

  // Witness always serves the other-audience scope — HandleAoIEnter
  // excludes &peer == &owner_, so owner-scope replication never flows
  // through here. The owner client receives its own deltas via the
  // CellAppNativeProvider direct path.
  if (have_continuous_coverage) {
    for (const auto& frame : state->history) {
      if (frame.event_seq < first_needed) continue;
      if (frame.event_seq > state->latest_event_seq) break;

      const auto& delta_bytes = frame.other_delta;
      // Skip empty / all-zero deltas — seq still advances so the next
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
          if (send_reliable_) send_reliable_(owner_.Id(), cached);
        }
        bytes += cached.size();
      }
      cache.last_event_seq = frame.event_seq;
    }
  } else {
    ATLAS_PROFILE_ZONE_N("Witness::Snapshot");
    // Snapshot fallback — observer fell out of the history window.
    // Carry latest_event_seq so the next delta seq+1 doesn't gap-warn.
    auto envelope = BuildPropertyUpdateEnvelope(cache.entity->Id(), state->latest_event_seq,
                                                state->other_snapshot);
    if (send_reliable_) send_reliable_(owner_.Id(), envelope);
    bytes += envelope.size();
    cache.last_event_seq = state->latest_event_seq;
  }
  return bytes;
}

}  // namespace atlas
