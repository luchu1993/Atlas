#ifndef ATLAS_SERVER_CELLAPP_WITNESS_H_
#define ATLAS_SERVER_CELLAPP_WITNESS_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "server/entity_types.h"

namespace atlas {

class CellEntity;
class AoITrigger;

// ============================================================================
// Witness — per-observer AoI replication manager
//
// Every CellEntity that acts as an AoI observer (today: every entity with
// a bound client; tomorrow: also AI "sensor" entities) owns one Witness.
// The witness plants an AoITrigger into the RangeList; entities entering
// and leaving the trigger produce EntityCache entries that the per-tick
// Witness::Update schedules for replication.
//
// Responsibilities:
//   - AoITrigger attaches and fires enter/leave events into the witness.
//   - An EntityCache state machine handles ENTER_PENDING, GONE, REFRESH.
//   - The priority heap (distance/5 + 1) orders per-tick work.
//   - Update emits CellAoIEnvelope-framed EntityEnter / EntityLeave events
//     through the provided delivery callback, plus event and volatile
//     delta streams keyed to CellEntity::ReplicationState seqs.
//
// Delivery interface:
//   The witness doesn't know about BaseApp or the network. Instead a
//   caller-supplied callback takes a CellAoIEnvelope blob and is
//   responsible for routing it (via SelfRpcFromCell /
//   ReplicatedReliableDeltaFromCell, whichever fits the payload's
//   reliability class). Tests plug a recording callback; the real
//   CellApp process plugs a BaseApp-forwarding callback.
// ============================================================================

class Witness {
 public:
  // Witness forwards envelopes through two transport classes — the
  // caller (CellApp process) wires each to the matching BaseApp message:
  //   - reliable: SelfRpcFromCell / ReplicatedReliableDeltaFromCell
  //               (property deltas, enters, leaves — order matters;
  //                dropping one permanently desyncs the client)
  //   - unreliable: ReplicatedDeltaFromCell (via DeltaForwarder, latest-
  //                wins — volatile position/orientation only)
  using SendFn =
      std::function<void(EntityID observer_base_id, std::span<const std::byte> envelope)>;

  // hysteresis widens the *leave* boundary: enters fire at `aoi_radius`,
  // leaves at `aoi_radius + hysteresis`. Pass 0.f to disable hysteresis
  // (single-band behaviour).
  Witness(CellEntity& owner, float aoi_radius, float hysteresis, SendFn send_reliable,
          SendFn send_unreliable = {});
  ~Witness();

  Witness(const Witness&) = delete;
  auto operator=(const Witness&) -> Witness& = delete;

  // Plant the trigger into the owning Space's RangeList and start
  // receiving enter/leave events. Returns self-pointer for caller
  // convenience; the owner CellEntity typically holds the unique_ptr.
  void Activate();

  // Tear down the trigger. Any remaining entries in aoi_map_ are dropped
  // WITHOUT firing OnLeave — the observer is (presumably) going away and
  // the client channel will be recycled by BaseApp.
  void Deactivate();

  [[nodiscard]] auto Owner() -> CellEntity& { return owner_; }
  [[nodiscard]] auto AoIRadius() const -> float { return aoi_radius_; }
  [[nodiscard]] auto Hysteresis() const -> float { return hysteresis_; }
  [[nodiscard]] auto IsActive() const -> bool { return trigger_ != nullptr; }

  // Resize the AoI radius + hysteresis in place. AoITrigger uses
  // RangeTrigger::SetRange's expand-first semantics on both bands, so
  // enters precede leaves when the new radius strictly contains or is
  // contained by the old one.
  void SetAoIRadius(float new_radius, float new_hysteresis);

  // Convenience overload — keeps the current hysteresis_ value. Useful
  // for call sites (and legacy tests) that only want to resize radius.
  void SetAoIRadius(float new_radius) { SetAoIRadius(new_radius, hysteresis_); }

  // Called by the AoITrigger when a peer crosses the trigger boundary.
  void HandleAoIEnter(CellEntity& peer);
  void HandleAoILeave(CellEntity& peer);

  // Drive the per-tick replication pump. max_packet_bytes bounds the
  // total envelope payload emitted this tick (not including outer
  // framing). Deficit accumulates when Update overshoots — the next
  // tick's budget is reduced accordingly.
  void Update(uint32_t max_packet_bytes);

  // ---- EntityCache (exposed for tests) -------------------------------------

  struct EntityCache {
    CellEntity* entity{nullptr};

    // Cached at AoI entry time so the EntityLeave envelope we emit at
    // compaction can survive the peer's destruction. When a peer is
    // destroyed while still inside AoI, the synthetic FLT_MAX shuffle
    // in `~CellEntity` fires OnLeave → HandleAoILeave (which marks
    // kGone on the cache) DURING the peer's destructor. Between that
    // moment and our next Update, the CellEntity is freed. Without a
    // cached id, `entity->BaseEntityId()` at Leave time is a UAF.
    EntityID peer_base_id{kInvalidEntityID};

    double priority{0.0};  // distance-based, smaller = higher priority
    uint8_t flags{0};

    // Per-observer replication progress. Witness compares these against
    // the entity's CellEntity::ReplicationState to decide what needs
    // forwarding this tick.
    uint64_t last_event_seq{0};
    uint64_t last_volatile_seq{0};

    static constexpr uint8_t kEnterPending = 0x01;  // just joined AoI
    static constexpr uint8_t kGone = 0x08;          // left AoI, pending leave send
    static constexpr uint8_t kRefresh = 0x10;       // resend snapshot (e.g. after radius change)

    [[nodiscard]] auto IsUpdatable() const -> bool {
      return (flags & (kEnterPending | kGone | kRefresh)) == 0;
    }
  };

  // Read-only access for tests.
  [[nodiscard]] auto AoIMap() const -> const std::unordered_map<EntityID, EntityCache>& {
    return aoi_map_;
  }
  [[nodiscard]] auto AoIMapMutable() -> std::unordered_map<EntityID, EntityCache>& {
    return aoi_map_;
  }
  // Drive the per-peer update pump once. Tests use this to assert
  // catch-up / snapshot-fallback behaviour without spinning the full
  // Update() tick loop.
  void TestOnlySendEntityUpdate(EntityCache& cache) { SendEntityUpdate(cache); }

 private:
  // Re-compute the priority (distance/5 + 1) using our current position.
  void UpdatePriority(EntityCache& cache) const;

  // Emit an EntityEnter envelope for `cache`. Uses owner.base_entity_id()
  // as the route key.
  void SendEntityEnter(EntityCache& cache);
  // Emit an EntityLeave envelope.
  void SendEntityLeave(EntityID peer_base_id);

  // Per-peer update pump — replays history delta OR falls back to
  // snapshot when the observer has dropped out of the history window.
  // Exposed via a helper so tests can invoke it directly on a live
  // cache, bypassing the full priority heap scheduling.
  void SendEntityUpdate(EntityCache& cache);

  CellEntity& owner_;
  float aoi_radius_;
  float hysteresis_;
  SendFn send_reliable_;
  SendFn send_unreliable_;

  std::unique_ptr<AoITrigger> trigger_;

  // Keyed by peer's base_entity_id (the stable id the client sees).
  // Using EntityID rather than CellEntity* keeps the reference weak;
  // if the peer gets destroyed its cache stays until the next Update
  // compacts it.
  std::unordered_map<EntityID, EntityCache> aoi_map_;

  // Min-heap of peer ids keyed on priority. IDs (not iterators / raw
  // pointers) so rehash of aoi_map_ can't dangle; every pop re-looks up.
  // std::pair<priority, id> makes std::greater ordering trivial.
  std::vector<std::pair<double, EntityID>> priority_queue_;

  // Bytes we went over budget last tick. Deducted from next tick's
  // effective budget so bursty Update work amortises evenly.
  int bandwidth_deficit_{0};

  // Scratch buffers for Update() — promoted from locals to avoid
  // per-tick heap allocation on a 10Hz hot path.
  std::vector<EntityID> scratch_enter_;
  std::vector<EntityID> scratch_gone_;
  std::vector<EntityID> scratch_refresh_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_WITNESS_H_
