#include "witness.h"

#include <algorithm>
#include <cstring>
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

// Distance-LOD thresholds (squared metres). Entities beyond each
// boundary are updated at a reduced rate to shed Witness::Update::Pump
// CPU and outbound bandwidth in dense scenes.
//
// Bands  (distance → update every N ticks at 10 Hz):
//   Close   < 25 m  →  1 tick  (10 Hz)
//   Medium  < 100 m →  3 ticks (~3.3 Hz)
//   Far     ≥ 100 m →  6 ticks (~1.7 Hz)
//
// At 10 Hz the Far interval (600 ms) comfortably fits inside the
// 8-frame history window (800 ms), so catch-up replay always covers
// the gap without falling back to snapshot.
static constexpr double kLodCloseSq = 25.0 * 25.0;     //    625 m²
static constexpr double kLodMediumSq = 100.0 * 100.0;  // 10 000 m²
static constexpr uint64_t kLodCloseInterval = 1;
static constexpr uint64_t kLodMediumInterval = 3;
static constexpr uint64_t kLodFarInterval = 6;

// Priority metric: squared distance. Smaller is more urgent. The
// min-heap in Update only cares about ordering, so we skip the sqrt
// that a true distance would require — a² < b² iff a < b for
// non-negative magnitudes, which distance always is. Saves one sqrt
// per updatable peer per tick.
auto ComputePriority(const math::Vector3& observer, const math::Vector3& target) -> double {
  const double dx = observer.x - target.x;
  const double dy = observer.y - target.y;
  const double dz = observer.z - target.z;
  return dx * dx + dy * dy + dz * dz;
}

// A per-audience delta with only the flag prefix and every byte zero
// means "no audience-visible field changed this frame" — usually
// produced when only owner-scope props were dirty but this side is
// serving the other-scope projection. Shipping it would burn a 1-4 B
// envelope to tell the client "nothing happened". Safe to skip because
// the client's seq catches up when the next non-empty frame arrives.
auto IsAllZeroDelta(std::span<const std::byte> delta) -> bool {
  return std::all_of(delta.begin(), delta.end(), [](std::byte b) { return b == std::byte{0}; });
}

// Encode CellAoIEnvelope { kind, public_entity_id, payload } into a byte
// buffer. Wire format, LE:
//   [uint8 kind] [uint32 LE public_entity_id] [variable payload bytes]
//
// Keeping this inline rather than routing through BinaryWriter avoids
// pulling a heavyweight dependency into the Witness hot path for what
// are trivially small messages.
template <std::size_t N>
auto MakeEnvelope(CellAoIEnvelopeKind kind, EntityID public_entity_id,
                  std::span<const std::byte> payload) -> std::vector<std::byte> {
  (void)N;  // placeholder in case we want a small-buffer optimisation
  std::vector<std::byte> out;
  out.reserve(1 + 4 + payload.size());
  out.push_back(static_cast<std::byte>(kind));
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::byte>((public_entity_id >> (i * 8)) & 0xFF));
  }
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

// Fused envelope builder for kEntityPropertyUpdate. Wire layout is
// [u8 kind][u32 LE entity_id][u64 LE event_seq][delta bytes...]. A
// two-helper composition (BuildPropertyUpdatePayload → MakeEnvelope)
// would allocate two vectors and copy the event_seq + delta bytes
// twice; this single-pass version packs directly into one buffer.
// Property updates are the hottest per-tick envelope (sent per
// observer per peer per frame), so the saving scales with aoi_map
// size × publish rate.
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

// Fused envelope builder for kEntityEnter. Wire layout:
//   [u8 kind][u32 LE entity_id][u16 LE type_id][3x float LE pos]
//   [3x float LE dir][u8 on_ground][peer_snapshot bytes...]
// Client decoder mirrors this in reverse. Single-pass memcpy into a
// pre-sized buffer avoids the 7 insert-shift operations the previous
// BuildEnterPayload→MakeEnvelope composition racked up per call.
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
  aoi_map_.clear();
  priority_queue_.clear();
}

void Witness::SetAoIRadius(float new_radius, float new_hysteresis) {
  // Clamp radius to a 0.1m floor and a configurable ceiling;
  // hysteresis is accepted as-given.
  new_radius = std::max(0.1f, new_radius);
  const float max_radius = CellAppConfig::MaxAoIRadius();
  if (new_radius > max_radius) {
    ATLAS_LOG_WARNING("Witness::SetAoIRadius: clamping entity {}'s AoI radius ({}) to max ({})",
                      owner_.BaseEntityId(), new_radius, max_radius);
    new_radius = max_radius;
  }
  aoi_radius_ = new_radius;
  hysteresis_ = std::max(0.f, new_hysteresis);
  if (trigger_) trigger_->SetBounds(aoi_radius_, aoi_radius_ + hysteresis_);
}

void Witness::HandleAoIEnter(CellEntity& peer) {
  if (&peer == &owner_) return;  // a witness never tracks its own central

  auto [it, inserted] = aoi_map_.try_emplace(peer.Id());
  auto& cache = it->second;

  // Dual-band hysteresis: inner's OnEnter fires every time a peer crosses
  // the inner boundary inbound — including re-crossings from within the
  // hysteresis band (outer > distance > inner). If the peer is already in
  // aoi_map_ and NOT flagged kGone, this is a hysteresis re-cross: the
  // client already sees the peer as in AoI, re-emitting a snapshot would
  // be wasteful and out-of-contract. Skip the state update.
  if (!inserted && (cache.flags & EntityCache::kGone) == 0) return;

  cache.entity = &peer;
  cache.peer_base_id = peer.BaseEntityId();  // safe-for-leave copy
  // ENTER_PENDING is "send a fresh snapshot to this observer" — set on
  // first entry and on any re-entry of a previously-kGone peer (the peer
  // left AoI and came back before Update had a chance to fire the Leave).
  cache.flags = EntityCache::kEnterPending;
  UpdatePriority(cache);
}

void Witness::HandleAoILeave(CellEntity& peer) {
  auto it = aoi_map_.find(peer.Id());
  if (it == aoi_map_.end()) return;
  // Mark GONE rather than erase immediately; the Update pass will fire
  // the actual EntityLeave envelope and then compact the entry. This
  // deferred erase keeps Witness::Update's iteration stable when a peer
  // leaves AoI mid-tick as a side effect of a trigger shuffle.
  it->second.flags |= EntityCache::kGone;
  it->second.flags &= ~EntityCache::kEnterPending;
  // Refresh the cached base id — if the Leave fires during the peer's
  // destructor (synthetic FLT_MAX shuffle) this is our last chance to
  // read it. After this call the CellEntity* may become dangling.
  it->second.peer_base_id = peer.BaseEntityId();
  // Null the entity pointer so any path that forgets to filter by
  // kGone before dereferencing gets a clean nullptr crash instead of
  // a use-after-free hidden by freed-memory coincidence.
  it->second.entity = nullptr;
}

void Witness::UpdatePriority(EntityCache& cache) const {
  cache.priority = ComputePriority(owner_.Position(), cache.entity->Position());
}

auto Witness::SendEntityEnter(EntityCache& cache) -> std::size_t {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityEnter");
  // Pin the peer locally — send_reliable_ below can re-entrantly destroy
  // entities (including this one), at which point HandleAoILeave nulls
  // cache.entity.  Reading through the local pointer keeps both the
  // pre-send envelope build and the post-send seq capture correct.
  CellEntity* const entity = cache.entity;

  // The snapshot we ship on Enter is the peer's *other* snapshot — the
  // observer is a non-owner for the peer (they see "other clients" data).
  // Owner-enter (the entity's own client observing itself) would use
  // owner_snapshot instead, but that path goes through a separate initial
  // state message, not the AoI enter flow.
  std::span<const std::byte> enter_snapshot{};
  uint64_t pre_event_seq = 0;
  uint64_t pre_volatile_seq = 0;
  if (const auto* state = entity->GetReplicationState()) {
    enter_snapshot = std::span<const std::byte>(state->other_snapshot);
    pre_event_seq = state->latest_event_seq;
    pre_volatile_seq = state->latest_volatile_seq;
  }

  auto envelope = BuildEnterEnvelope(entity->BaseEntityId(), entity->TypeId(), entity->Position(),
                                     entity->Direction(), entity->OnGround(), enter_snapshot);
  if (send_reliable_) send_reliable_(owner_.BaseEntityId(), envelope);

  // After send_reliable_ returns, the peer may have been destroyed
  // re-entrantly — but `cache` is a reference into aoi_map_ which is
  // stable under HandleAoILeave (no insertions, just flag/pointer
  // mutations).  Only stamp the seqs if the entity wasn't yanked.
  if (cache.entity == entity) {
    cache.last_event_seq = pre_event_seq;
    cache.last_volatile_seq = pre_volatile_seq;
  }
  return envelope.size();
}

auto Witness::SendEntityLeave(EntityID peer_base_id) -> std::size_t {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityLeave");
  auto envelope = MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityLeave, peer_base_id, {});
  if (send_reliable_) send_reliable_(owner_.BaseEntityId(), envelope);
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

  // Step 1: state transitions (enter, leave). Collect peers into scratch
  // lists so mutations to aoi_map_ during this pass (possible if SendFn
  // re-entrantly tears down the witness) don't invalidate iterators.
  // Scratch buffers are class members to avoid per-tick allocation.
  int bytes_sent = 0;
  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::Transitions");
    scratch_enter_.clear();
    scratch_gone_.clear();
    auto& enter_ids = scratch_enter_;
    auto& gone_ids = scratch_gone_;
    for (auto& [id, cache] : aoi_map_) {
      if (cache.flags & EntityCache::kGone)
        gone_ids.push_back(id);
      else if (cache.flags & EntityCache::kEnterPending)
        enter_ids.push_back(id);
    }

    // Enter/Leave are mandatory state transitions and bypass the byte
    // budget — dropping them would deadlock the aoi_map_ state machine.
    (void)bandwidth_deficit_;

    for (std::size_t enter_idx = 0; enter_idx < enter_ids.size(); ++enter_idx) {
      auto it = aoi_map_.find(enter_ids[enter_idx]);
      if (it == aoi_map_.end()) continue;
      auto& cache = it->second;
      // Re-check: send_reliable_ from a previous iteration may have
      // re-entrantly destroyed this peer, in which case HandleAoILeave
      // has cleared kEnterPending and nulled cache.entity.  Don't
      // SendEntityEnter on a freed peer — the gone_ids loop below will
      // emit the matching Leave envelope.
      if (!(cache.flags & EntityCache::kEnterPending) || !cache.entity) continue;
      bytes_sent += static_cast<int>(SendEntityEnter(cache));
      cache.flags &= ~EntityCache::kEnterPending;
      cache.lod_enter_phase = enter_idx % kLodFarInterval;
    }

    for (auto id : gone_ids) {
      auto it = aoi_map_.find(id);
      if (it == aoi_map_.end()) continue;
      // Use the id cached at enter/leave time, NOT cache.entity. When
      // destruction fires the leave path, `cache.entity` is nulled and
      // the underlying CellEntity has been freed; the base id we want
      // to ship to the client was captured earlier. See EntityCache's
      // comment on `peer_base_id`.
      bytes_sent += static_cast<int>(SendEntityLeave(it->second.peer_base_id));
      aoi_map_.erase(it);
    }
  }

  // Step 2: priority heap maintenance for Updatable peers. Rebuild rather
  // than maintain incrementally — observer positions change every tick so
  // priorities are stale anyway.
  //
  // LOD gate: skip peers whose scheduled next-update tick hasn't arrived.
  // lod_next_update_tick starts at 0 so every peer is eligible on the
  // first tick; after each SendEntityUpdate it is reset to
  // tick_count_ + LodIntervalForDistSq(distance²).
  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::PriorityHeap");
    priority_queue_.clear();
    priority_queue_.reserve(aoi_map_.size());
    for (auto& [id, cache] : aoi_map_) {
      if (!cache.IsUpdatable()) continue;
      if (tick_count_ < cache.lod_next_update_tick) continue;  // LOD skip
      UpdatePriority(cache);
      priority_queue_.emplace_back(cache.priority, id);
    }
    // Min-heap on priority: use std::greater so pop_heap yields smallest
    // priority first.
    std::make_heap(priority_queue_.begin(), priority_queue_.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  }

  // Step 3: per-peer update pump — property deltas + volatile position.
  // Walk the heap in priority order; SendEntityUpdate picks between
  // catch-up delta, snapshot fallback, or a volatile position update.
  // The budget gates how many peers we service this tick; any that didn't
  // fit carry over (priority stays put, so next tick they float up).
  {
    ATLAS_PROFILE_ZONE_N("Witness::Update::Pump");
    const int tick_budget = static_cast<int>(max_packet_bytes) - bandwidth_deficit_;
    // Hard cap on peers serviced this tick — caps serialisation CPU even when
    // the byte budget would allow more. Read once per Update; the config knob
    // is RW so ops can retune live without rebuild.
    const std::size_t max_peers = CellAppConfig::WitnessMaxPeersPerTick();
    std::size_t peers_updated = 0;
    while (!priority_queue_.empty() && bytes_sent < tick_budget && peers_updated < max_peers) {
      std::pop_heap(priority_queue_.begin(), priority_queue_.end(),
                    [](const auto& a, const auto& b) { return a.first > b.first; });
      const auto [prio, id] = priority_queue_.back();
      priority_queue_.pop_back();

      auto it = aoi_map_.find(id);
      if (it == aoi_map_.end()) continue;  // evicted mid-loop
      auto& cache = it->second;
      if (!cache.IsUpdatable()) continue;

      bytes_sent += static_cast<int>(SendEntityUpdate(cache));
      ++peers_updated;
      // Schedule the next LOD window. lod_enter_phase is non-zero only on
      // the first schedule (set at AoI-enter, cleared here); it offsets the
      // window by up to kLodFarInterval-1 ticks to stagger simultaneous
      // entries. % interval keeps the offset within one window regardless of
      // which band the peer is in; Close (interval=1) always yields 0.
      //
      // Note: the very first delta after AoI-enter therefore lands at
      // tick_count_ + interval + offset, i.e. up to interval + (kLodFarInterval-1)
      // ticks later (worst case Far = 11 ticks ≈ 1.1 s at 10 Hz). Subsequent
      // windows fall back to the regular tick_count_ + interval cadence.
      const uint64_t interval = LodIntervalForDistSq(cache.priority);
      cache.lod_next_update_tick = tick_count_ + interval + (cache.lod_enter_phase % interval);
      cache.lod_enter_phase = 0;
    }
  }

  // Step 4: bandwidth deficit for next tick.
  bandwidth_deficit_ = std::max(0, bytes_sent - static_cast<int>(max_packet_bytes));

  // Deficit larger than a whole tick's budget means the next tick alone
  // cannot drain it — the witness is structurally overloaded (too many
  // peers, too-small budget, or a state burst). Warn rate-limited so a
  // sustained issue logs once per ~10s at 30Hz rather than every tick.
  if (bandwidth_deficit_ > static_cast<int>(max_packet_bytes)) {
    if (deficit_warn_counter_ == 0) {
      ATLAS_LOG_WARNING(
          "Witness[{}]: bandwidth deficit {}B > per-observer budget {}B; peer catch-up "
          "will lag. Consider increasing cellapp/witness_per_observer_budget_bytes.",
          owner_.BaseEntityId(), bandwidth_deficit_, max_packet_bytes);
    }
    if (++deficit_warn_counter_ >= kDeficitWarnEveryNTicks) deficit_warn_counter_ = 0;
  } else {
    deficit_warn_counter_ = 0;
  }
}

// SendEntityUpdate — per-peer catch-up pump. Pulls two streams off the
// peer's ReplicationState: volatile (latest-wins position update) and
// event (ordered, replays history from last_event_seq+1..latest; snapshot
// fallback when the window no longer covers the observer's gap).
auto Witness::SendEntityUpdate(EntityCache& cache) -> std::size_t {
  ATLAS_PROFILE_ZONE_N("Witness::SendEntityUpdate");
  const auto* state = cache.entity->GetReplicationState();
  if (!state) return 0;

  std::size_t bytes = 0;

  // ---- Volatile stream ----
  if (state->latest_volatile_seq > cache.last_volatile_seq) {
    // Build the EntityPositionUpdate wire envelope into a stack buffer.
    // Pre-fix this path took two heap allocations per call (one for the
    // payload buffer, one for the envelope vector returned by
    // MakeEnvelope); at 200 obs × ~30 visible peers × 10 Hz the alloc
    // overhead alone dominated the SendEntityUpdate hot path. The
    // lambda consumes a span via ReplicatedDeltaFromCellSpan, so this
    // stack buffer is the single source of truth for the wire bytes —
    // it must outlive the synchronous send_unreliable_ / send_reliable_
    // call (it does: same scope).
    //
    // Wire layout (30 bytes):
    //   [u8 kind][u32 entity_id][3f pos][3f dir][u8 on_ground]
    constexpr std::size_t kEnvelopeSize = 1 + sizeof(uint32_t) + 6 * sizeof(float) + 1;
    std::array<std::byte, kEnvelopeSize> envelope_buf;
    std::size_t envelope_size = 0;
    {
      ATLAS_PROFILE_ZONE_N("Witness::Vol::Build");
      const auto& pos = cache.entity->Position();
      const auto& dir = cache.entity->Direction();
      const uint8_t og = cache.entity->OnGround() ? 1 : 0;
      const EntityID public_eid = cache.entity->BaseEntityId();

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
      // Volatile → unreliable path when wired; fall back to reliable only
      // because the integration-layer caller may have left send_unreliable_
      // unset (tests typically do).
      if (send_unreliable_) {
        send_unreliable_(owner_.BaseEntityId(), envelope);
      } else if (send_reliable_) {
        send_reliable_(owner_.BaseEntityId(), envelope);
      }
    }
    bytes += envelope.size();
    cache.last_volatile_seq = state->latest_volatile_seq;
  }

  // ---- Event stream ----
  if (state->latest_event_seq <= cache.last_event_seq) return bytes;  // up to date

  // Decide: can we replay from history, or do we need snapshot fallback?
  //
  // history holds frames with event_seq values — by construction in
  // PublishReplicationFrame, those values are consecutive (we only push
  // frames whose event_seq equals latest_event_seq at the time of the
  // call, which increases by 1 each publish). So "can we cover the
  // observer's gap" boils down to "does history contain a frame with
  // event_seq == cache.last_event_seq + 1"?
  const uint64_t first_needed = cache.last_event_seq + 1;
  const bool have_continuous_coverage =
      !state->history.empty() && state->history.front().event_seq <= first_needed;

  // Observer is audience=owner if owner_.base_entity_id() matches the
  // peer's base_entity_id (i.e. the peer's client is THIS observer).
  // Otherwise audience=other — use the peer's other_snapshot / other_delta.
  const bool observer_is_owner = cache.entity->BaseEntityId() == owner_.BaseEntityId();

  if (have_continuous_coverage) {
    // Walk forward through history; skip entries older than first_needed
    // (they were already delivered earlier).
    for (const auto& frame : state->history) {
      if (frame.event_seq < first_needed) continue;
      if (frame.event_seq > state->latest_event_seq) break;  // defensive

      const auto& delta_bytes = observer_is_owner ? frame.owner_delta : frame.other_delta;
      // Skip the send when either the delta is truly empty OR the flag
      // prefix is all-zero (no audience-visible fields touched). Seq
      // still advances so the next non-empty frame doesn't look like a
      // gap.
      if (!delta_bytes.empty() && !IsAllZeroDelta(delta_bytes)) {
        // Reuse the cached envelope if already built by an earlier observer
        // this tick. mutable cache on ReplicationFrame lets us share the
        // allocation across all N witnesses watching the same peer.
        auto& cached =
            observer_is_owner ? frame.cached_owner_envelope : frame.cached_other_envelope;
        if (cached.empty()) {
          ATLAS_PROFILE_ZONE_N("Witness::Event::Build");
          cached = BuildPropertyUpdateEnvelope(cache.entity->BaseEntityId(), frame.event_seq,
                                               delta_bytes);
        }
        {
          ATLAS_PROFILE_ZONE_N("Witness::Event::Send");
          if (send_reliable_) send_reliable_(owner_.BaseEntityId(), cached);
        }
        bytes += cached.size();
      }
      cache.last_event_seq = frame.event_seq;
    }
  } else {
    ATLAS_PROFILE_ZONE_N("Witness::Snapshot");
    // Snapshot fallback — our observer fell too far behind and the
    // oldest history frame is newer than last_event_seq+1. Ship the
    // current audience-scope snapshot; the client resets its view of
    // the peer and resumes catch-up from there.
    const auto& snapshot = observer_is_owner ? state->owner_snapshot : state->other_snapshot;
    // Carry latest_event_seq as the envelope's seq: after snapshot apply
    // the client's "last seen" seq moves forward to the publishing frame.
    // The next delta with seq = latest_event_seq+1 will not trigger a gap
    // warning.
    auto envelope = BuildPropertyUpdateEnvelope(cache.entity->BaseEntityId(),
                                                state->latest_event_seq, snapshot);
    if (send_reliable_) send_reliable_(owner_.BaseEntityId(), envelope);
    bytes += envelope.size();
    cache.last_event_seq = state->latest_event_seq;
  }
  return bytes;
}

}  // namespace atlas
