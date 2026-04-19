#include "witness.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "aoi_trigger.h"
#include "cell_aoi_envelope.h"
#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "space/range_list.h"

namespace atlas {

namespace {

// Priority metric: distance / 5 + 1. Smaller is more urgent. Matches
// BigWorld's default; tune per game once telemetry exists.
auto ComputePriority(const math::Vector3& observer, const math::Vector3& target) -> double {
  const float dx = observer.x - target.x;
  const float dy = observer.y - target.y;
  const float dz = observer.z - target.z;
  const auto dist = static_cast<double>(std::sqrt(dx * dx + dy * dy + dz * dz));
  return dist / 5.0 + 1.0;
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

// Payload for EntityEnter: type_id (uint16 LE) + position (3x float LE)
// + direction (3x float LE) + on_ground (uint8). Client decoder mirrors
// this in the reverse order. Owner snapshot bytes (if any) append after.
auto BuildEnterPayload(uint16_t type_id, const math::Vector3& pos, const math::Vector3& dir,
                       bool on_ground, std::span<const std::byte> owner_snapshot)
    -> std::vector<std::byte> {
  std::vector<std::byte> payload;
  payload.reserve(2 + 6 * sizeof(float) + 1 + owner_snapshot.size());
  auto push = [&payload](const void* src, std::size_t n) {
    const auto* bytes = static_cast<const std::byte*>(src);
    payload.insert(payload.end(), bytes, bytes + n);
  };
  push(&type_id, sizeof(type_id));
  push(&pos.x, sizeof(float));
  push(&pos.y, sizeof(float));
  push(&pos.z, sizeof(float));
  push(&dir.x, sizeof(float));
  push(&dir.y, sizeof(float));
  push(&dir.z, sizeof(float));
  const uint8_t og = on_ground ? 1 : 0;
  payload.push_back(static_cast<std::byte>(og));
  payload.insert(payload.end(), owner_snapshot.begin(), owner_snapshot.end());
  return payload;
}

}  // namespace

Witness::Witness(CellEntity& owner, float aoi_radius, SendFn send_reliable, SendFn send_unreliable)
    : owner_(owner),
      aoi_radius_(aoi_radius),
      send_reliable_(std::move(send_reliable)),
      send_unreliable_(std::move(send_unreliable)) {}

Witness::~Witness() = default;

void Witness::Activate() {
  if (trigger_) return;
  trigger_ = std::make_unique<AoITrigger>(*this, owner_.RangeNode(), aoi_radius_);
  trigger_->Insert(owner_.GetSpace().GetRangeList());
}

void Witness::Deactivate() {
  if (!trigger_) return;
  trigger_->Remove(owner_.GetSpace().GetRangeList());
  trigger_.reset();
  aoi_map_.clear();
  priority_queue_.clear();
}

void Witness::SetAoIRadius(float new_radius) {
  aoi_radius_ = new_radius;
  if (trigger_) trigger_->SetRange(new_radius);
}

void Witness::HandleAoIEnter(CellEntity& peer) {
  if (&peer == &owner_) return;  // a witness never tracks its own central

  auto [it, inserted] = aoi_map_.try_emplace(peer.Id());
  auto& cache = it->second;
  cache.entity = &peer;
  cache.peer_base_id = peer.BaseEntityId();  // safe-for-leave copy
  // ENTER_PENDING is "send a fresh snapshot to this observer" — set on
  // first entry and on any readd-after-leave. Clear GONE if a peer left
  // and re-entered within the same tick (cache survived the gone sweep).
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

void Witness::SendEntityEnter(EntityCache& cache) {
  // The snapshot we ship on Enter is the peer's *other* snapshot — the
  // observer is a non-owner for the peer (they see "other clients" data).
  // Owner-enter (the entity's own client observing itself) would use
  // owner_snapshot instead, but that path goes through a separate initial
  // state message, not the AoI enter flow.
  std::span<const std::byte> enter_snapshot{};
  if (const auto* state = cache.entity->GetReplicationState()) {
    enter_snapshot = std::span<const std::byte>(state->other_snapshot);
  }

  auto payload =
      BuildEnterPayload(cache.entity->TypeId(), cache.entity->Position(), cache.entity->Direction(),
                        cache.entity->OnGround(), enter_snapshot);
  auto envelope =
      MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityEnter, cache.entity->BaseEntityId(), payload);
  if (send_reliable_) send_reliable_(owner_.BaseEntityId(), envelope);

  // Capture the peer's current seqs so 10.5b's catch-up pump only
  // replays events that happened AFTER this enter.
  if (const auto* state = cache.entity->GetReplicationState()) {
    cache.last_event_seq = state->latest_event_seq;
    cache.last_volatile_seq = state->latest_volatile_seq;
  }
}

void Witness::SendEntityLeave(EntityID peer_base_id) {
  auto envelope = MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityLeave, peer_base_id, {});
  if (send_reliable_) send_reliable_(owner_.BaseEntityId(), envelope);
}

void Witness::Update(uint32_t max_packet_bytes) {
  if (!trigger_) return;

  // Phase 1: state transitions (enter, refresh, leave). Collect peers
  // to process into a temp list so mutations to aoi_map_ during this
  // pass (unlikely, but possible if SendFn re-entrantly tears down the
  // witness) don't invalidate iterators. Scratch buffers are class
  // members to avoid per-tick allocation on a 10Hz hot path.
  scratch_enter_.clear();
  scratch_gone_.clear();
  scratch_refresh_.clear();
  auto& enter_ids = scratch_enter_;
  auto& gone_ids = scratch_gone_;
  auto& refresh_ids = scratch_refresh_;
  for (auto& [id, cache] : aoi_map_) {
    if (cache.flags & EntityCache::kGone)
      gone_ids.push_back(id);
    else if (cache.flags & EntityCache::kEnterPending)
      enter_ids.push_back(id);
    else if (cache.flags & EntityCache::kRefresh)
      refresh_ids.push_back(id);
  }

  // budget is computed for Phase 10.5b's priority-heap pump (below);
  // for the 10.5a skeleton the Enter/Leave/Refresh passes don't yet
  // honour it — Enter/Leave are mandatory state transitions that would
  // deadlock if budget-dropped.
  (void)bandwidth_deficit_;  // consumed in 10.5b
  int bytes_sent = 0;

  for (auto id : enter_ids) {
    auto it = aoi_map_.find(id);
    if (it == aoi_map_.end()) continue;
    auto& cache = it->second;
    SendEntityEnter(cache);
    cache.flags &= ~EntityCache::kEnterPending;
    // Tiny but non-trivial envelope — count a conservative size; 10.5b
    // will compute actual bytes via the SpanWriter we're skipping today.
    bytes_sent += 16;
  }

  for (auto id : refresh_ids) {
    auto it = aoi_map_.find(id);
    if (it == aoi_map_.end()) continue;
    auto& cache = it->second;
    SendEntityEnter(cache);  // refresh shape ≡ re-send snapshot-bearing enter
    cache.flags &= ~EntityCache::kRefresh;
    bytes_sent += 16;
  }

  for (auto id : gone_ids) {
    auto it = aoi_map_.find(id);
    if (it == aoi_map_.end()) continue;
    // Use the id cached at enter/leave time, NOT cache.entity. When
    // destruction fires the leave path, `cache.entity` is nulled and
    // the underlying CellEntity has been freed; the base id we want
    // to ship to the client was captured earlier. See EntityCache's
    // comment on `peer_base_id`.
    SendEntityLeave(it->second.peer_base_id);
    aoi_map_.erase(it);
    bytes_sent += 5;
  }

  // Phase 2: priority heap maintenance for Updatable peers. Rebuild
  // rather than maintain incrementally — observer positions change every
  // tick so priorities are stale anyway.
  priority_queue_.clear();
  priority_queue_.reserve(aoi_map_.size());
  for (auto& [id, cache] : aoi_map_) {
    if (!cache.IsUpdatable()) continue;
    UpdatePriority(cache);
    priority_queue_.emplace_back(cache.priority, id);
  }
  // Min-heap on priority: use std::greater so pop_heap yields smallest
  // priority first.
  std::make_heap(priority_queue_.begin(), priority_queue_.end(),
                 [](const auto& a, const auto& b) { return a.first > b.first; });

  // Phase 3: per-peer update pump — property deltas + volatile position.
  // Walk the heap in priority order; each entity gets SendEntityUpdate
  // which decides whether to forward catch-up delta, snapshot fallback,
  // or a volatile position update. The budget gates how many peers we
  // service this tick; any that didn't fit carry over (their priority
  // stays where it was, so next tick they float up).
  const int tick_budget = static_cast<int>(max_packet_bytes) - bandwidth_deficit_;
  while (!priority_queue_.empty() && bytes_sent < tick_budget) {
    std::pop_heap(priority_queue_.begin(), priority_queue_.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
    const auto [prio, id] = priority_queue_.back();
    priority_queue_.pop_back();

    auto it = aoi_map_.find(id);
    if (it == aoi_map_.end()) continue;  // evicted mid-loop
    auto& cache = it->second;
    if (!cache.IsUpdatable()) continue;

    // The SendEntityUpdate calls may re-enter send_* which records bytes
    // at the transport layer; we approximate with envelope sizes here.
    // Precise accounting arrives with the SpanWriter-based encoder.
    const std::size_t before = aoi_map_.size();
    SendEntityUpdate(cache);
    bytes_sent += 32;  // placeholder per-peer cost; revisit in §3.11 pass
    (void)before;
  }

  // Phase 4: bandwidth deficit for next tick.
  bandwidth_deficit_ = std::max(0, bytes_sent - static_cast<int>(max_packet_bytes));
}

// ---------------------------------------------------------------------------
// SendEntityUpdate — per-peer catch-up pump
// ---------------------------------------------------------------------------
//
// Pulls two streams off the peer's ReplicationState:
//   1) Volatile (latest-wins): bump last_volatile_seq in one go to the
//      peer's current latest, sending a single EntityPositionUpdate
//      envelope with the peer's current pos/dir/on_ground.
//   2) Event (ordered, cumulative): try to replay every history frame
//      from last_event_seq+1 through latest_event_seq. If the window
//      doesn't cover the full range (because the peer dropped older
//      frames), fall back to shipping the other-scope snapshot.
//
// Phase 10.5a's test helpers invoke this directly; Phase 10.5b's
// priority-heap walk in Update() invokes it every tick per scheduled
// peer.
//
void Witness::SendEntityUpdate(EntityCache& cache) {
  const auto* state = cache.entity->GetReplicationState();
  if (!state) return;

  // ---- Volatile stream ----
  if (state->latest_volatile_seq > cache.last_volatile_seq) {
    // Build an EntityPositionUpdate envelope: pos (3f) + dir (3f) + on_ground (u8).
    const auto& pos = cache.entity->Position();
    const auto& dir = cache.entity->Direction();
    const uint8_t og = cache.entity->OnGround() ? 1 : 0;

    std::vector<std::byte> payload;
    payload.reserve(6 * sizeof(float) + 1);
    auto push = [&payload](const void* src, std::size_t n) {
      const auto* bytes = static_cast<const std::byte*>(src);
      payload.insert(payload.end(), bytes, bytes + n);
    };
    push(&pos.x, sizeof(float));
    push(&pos.y, sizeof(float));
    push(&pos.z, sizeof(float));
    push(&dir.x, sizeof(float));
    push(&dir.y, sizeof(float));
    push(&dir.z, sizeof(float));
    payload.push_back(static_cast<std::byte>(og));

    auto envelope = MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityPositionUpdate,
                                    cache.entity->BaseEntityId(), payload);
    // Volatile → unreliable path when wired; fall back to reliable only
    // because the integration-layer caller may have left send_unreliable_
    // unset (tests typically do).
    if (send_unreliable_) {
      send_unreliable_(owner_.BaseEntityId(), envelope);
    } else if (send_reliable_) {
      send_reliable_(owner_.BaseEntityId(), envelope);
    }
    cache.last_volatile_seq = state->latest_volatile_seq;
  }

  // ---- Event stream ----
  if (state->latest_event_seq <= cache.last_event_seq) return;  // up to date

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
      // Even "empty" per-audience deltas still carry PR-C's leading flag
      // byte, so we always ship something when a frame falls into our
      // range — suppress entirely only if the peer genuinely had no
      // audience-visible change this frame.
      if (!delta_bytes.empty()) {
        auto envelope =
            MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityPropertyUpdate,
                            cache.entity->BaseEntityId(), std::span<const std::byte>(delta_bytes));
        if (send_reliable_) send_reliable_(owner_.BaseEntityId(), envelope);
      }
      cache.last_event_seq = frame.event_seq;
    }
  } else {
    // Snapshot fallback — our observer fell too far behind and the
    // oldest history frame is newer than last_event_seq+1. Ship the
    // current audience-scope snapshot; the client resets its view of
    // the peer and resumes catch-up from there.
    const auto& snapshot = observer_is_owner ? state->owner_snapshot : state->other_snapshot;
    auto envelope =
        MakeEnvelope<0>(CellAoIEnvelopeKind::kEntityPropertyUpdate, cache.entity->BaseEntityId(),
                        std::span<const std::byte>(snapshot));
    if (send_reliable_) send_reliable_(owner_.BaseEntityId(), envelope);
    cache.last_event_seq = state->latest_event_seq;
    cache.flags &= ~EntityCache::kRefresh;
  }
}

}  // namespace atlas
