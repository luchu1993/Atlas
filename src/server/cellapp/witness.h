#ifndef ATLAS_SERVER_CELLAPP_WITNESS_H_
#define ATLAS_SERVER_CELLAPP_WITNESS_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include "intercell_messages.h"
#include "server/entity_types.h"
#include "space_data.h"

namespace atlas {

class CellEntity;
class AoITrigger;
class Channel;

// Per-observer AoI replication manager. Plants an AoITrigger into the
// owning Space's RangeList; the trigger feeds an EntityCache state
// machine; Update() drains the per-tick replication queue and emits
// CellAoIEnvelope-framed messages through caller-supplied transports
// (reliable for state transitions / event deltas, unreliable for
// volatile position).
class Witness {
 public:
  // reliable carries enters, leaves, and event deltas; a dropped
  // reliable envelope desyncs the client permanently. unreliable is
  // optional (volatile position) and falls back to reliable when unset.
  // The callback receives the envelope only; the observer-side routing
  // hint (channel, observer id) is captured by the producer at install.
  using SendFn = std::function<void(std::span<const std::byte> envelope)>;

  // hysteresis widens the leave boundary: enters fire at aoi_radius,
  // leaves at aoi_radius + hysteresis. Pass 0.f for single-band.
  Witness(CellEntity& owner, float aoi_radius, float hysteresis, SendFn send_reliable,
          SendFn send_unreliable = {});
  ~Witness();

  Witness(const Witness&) = delete;
  auto operator=(const Witness&) -> Witness& = delete;

  void Activate();

  // flush_leaves=false during ~CellEntity (callback re-entry) and Offload
  // (the destination Witness diffs against the inherited AoI).
  void Deactivate(bool flush_leaves = true);

  // Per-peer Witness snapshot for Offload migration (LOD + seqs included
  // so the destination resumes mid-stream).
  [[nodiscard]] auto SerializeAoI() const -> std::vector<cellapp::WitnessAoIEntry>;

  // Post-Activate diff: overlaps drop pending-Enter and adopt the
  // inherited LOD/seqs; orphans ship Leave.
  void InheritAoIFrom(const std::vector<cellapp::WitnessAoIEntry>& inherited);

  [[nodiscard]] auto Owner() -> CellEntity& { return owner_; }
  [[nodiscard]] auto AoIRadius() const -> float { return aoi_radius_; }
  [[nodiscard]] auto Hysteresis() const -> float { return hysteresis_; }
  [[nodiscard]] auto IsActive() const -> bool { return trigger_ != nullptr; }

  void SetAoIRadius(float new_radius, float new_hysteresis);
  void SetAoIRadius(float new_radius) { SetAoIRadius(new_radius, hysteresis_); }

  void HandleAoIEnter(CellEntity& peer);
  void HandleAoILeave(CellEntity& peer);

  // Resync trigger bounds after the owner's range_node has shuffled.
  // Without it inside_peers_ goes stale and OnLeave fails to fire for
  // peers the observer has drifted away from.
  void OnOwnerMoved(float old_x, float old_z);

  void ForceOuterInsidePeer(class RangeListNode& peer);

  struct UpdateStats {
    uint64_t reliable_bytes{0};
    uint64_t unreliable_bytes{0};
    [[nodiscard]] auto TotalBytes() const -> uint64_t {
      return reliable_bytes + unreliable_bytes;
    }
  };

  // max_packet_bytes bounds this tick; overshoot shrinks the next tick's budget.
  auto Update(uint32_t max_packet_bytes) -> UpdateStats;

  // O(1) demand estimate for the cellapp's fair-share budget allocator.
  // Enter bursts aren't separately accounted for - they show up next
  // tick via bandwidth_deficit_, costing one tick of lag.
  [[nodiscard]] auto EstimateOutboundDemandBytes(uint32_t per_peer_bytes) const -> uint32_t {
    return static_cast<uint32_t>(aoi_map_.size()) * per_peer_bytes +
           static_cast<uint32_t>(std::max(0, bandwidth_deficit_));
  }

  struct EntityCache {
    CellEntity* entity{nullptr};

    double priority{0.0};  // squared distance, smaller = higher priority
    uint8_t flags{0};

    uint64_t last_event_seq{0};
    uint64_t last_volatile_seq{0};

    // Peer is excluded from the priority queue until tick_count_
    // reaches this value; reset after each SendEntityUpdate based on
    // the current distance band.
    uint64_t lod_next_update_tick{0};

    // One-shot offset applied at AoI-enter to stagger peers entering
    // on the same tick across the LOD interval; cleared after the
    // first SendEntityUpdate.
    uint64_t lod_enter_phase{0};

    // Wall tick of the last SendEntityUpdate; rank-cut promotes peers
    // past witness_lod_starvation_threshold_ticks so a band that's
    // permanently over-cap still services everyone.
    uint64_t last_serviced_tick{0};

    static constexpr uint8_t kEnterPending = 0x01;
    static constexpr uint8_t kGone = 0x08;

    [[nodiscard]] auto IsUpdatable() const -> bool {
      return (flags & (kEnterPending | kGone)) == 0;
    }
  };

  [[nodiscard]] auto AoIMap() const -> const std::unordered_map<EntityID, EntityCache>& {
    return aoi_map_;
  }
  [[nodiscard]] auto AoIMapMutable() -> std::unordered_map<EntityID, EntityCache>& {
    return aoi_map_;
  }
  void TestOnlySendEntityUpdate(EntityCache& cache) { (void)SendEntityUpdate(cache); }
  [[nodiscard]] auto LastUpdateStats() const -> UpdateStats { return last_update_stats_; }

  // Space-scoped envelope pushes. Routed through send_reliable_ — same
  // channel as kEntityEnter so client ordering with peer arrival is intact.
  void SendSpaceDataInit(SpaceID space_id, const SpaceData& data);
  void SendSpaceDataUpdate(SpaceID space_id, uint16_t key_id,
                           std::span<const uint8_t> value);
  void SendSpaceDataDelete(SpaceID space_id, uint16_t key_id);

  [[nodiscard]] auto OutboundChannel() const -> Channel* { return outbound_channel_; }
  void SetOutboundChannel(Channel* ch) { outbound_channel_ = ch; }

 private:
  void UpdatePriority(EntityCache& cache) const;

  // Each Send* returns bytes actually dispatched so the tick-loop can bill precisely.
  auto SendEntityEnter(EntityCache& cache) -> UpdateStats;
  auto SendEntityLeave(EntityID peer_id) -> UpdateStats;
  auto SendEntityUpdate(EntityCache& cache) -> UpdateStats;

  CellEntity& owner_;
  float aoi_radius_;
  float hysteresis_;
  SendFn send_reliable_;
  SendFn send_unreliable_;

  // Matches the Channel* captured by the send callbacks.
  Channel* outbound_channel_{nullptr};

  std::unique_ptr<AoITrigger> trigger_;

  // Keyed by EntityID rather than CellEntity* so a peer destroyed
  // mid-tick can't dangle the cache; every Update re-looks-up.
  std::unordered_map<EntityID, EntityCache> aoi_map_;

  // Scratch storage for the per-band rank cut + sort during Update;
  // each band reuses the same vector to avoid per-tick allocations.
  std::vector<std::pair<double, EntityID>> band_scratch_;

  uint64_t tick_count_{0};

  // Bytes over budget last tick; deducted from next tick's budget.
  int bandwidth_deficit_{0};
  UpdateStats last_update_stats_;

  // Rate-limit overload warnings: a sustained deficit logs once and
  // stays quiet rather than spamming every tick.
  uint32_t deficit_warn_counter_{0};
  static constexpr uint32_t kDeficitWarnEveryNTicks = 300;

  // Populated by HandleAoIEnter / HandleAoILeave, drained by Update.
  // Duplicates tolerated - the drain loop's flag check filters stale
  // entries (e.g. an Enter overridden by a later Leave).
  std::vector<EntityID> pending_enter_ids_;
  std::vector<EntityID> pending_gone_ids_;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_WITNESS_H_
