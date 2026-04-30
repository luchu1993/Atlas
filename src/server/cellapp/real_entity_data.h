#ifndef ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_
#define ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "cellapp/intercell_messages.h"
#include "foundation/clock.h"
#include "math/vector3.h"
#include "network/address.h"

namespace atlas {

class CellEntity;
class Channel;

// Real-only sidecar: Haunt list (peer CellApps holding a Ghost), velocity
// sample, broadcast bookkeeping. Dropped on Real->Ghost transition.
// Channel* pointers are non-owning.
class RealEntityData {
 public:
  struct Haunt {
    Channel* channel{nullptr};
    Address addr{};
    TimePoint creation_time{};
  };

  explicit RealEntityData(CellEntity& owner);
  ~RealEntityData();

  RealEntityData(const RealEntityData&) = delete;
  auto operator=(const RealEntityData&) -> RealEntityData& = delete;

  // Idempotent; returns true on insert. addr cached so GhostMaintainer
  // matches by Address (survives peer reconnect rebinds Channel*).
  auto AddHaunt(Channel* channel, const Address& addr) -> bool;

  auto RemoveHaunt(Channel* channel) -> bool;

  [[nodiscard]] auto HasHaunt(Channel* channel) const -> bool;
  [[nodiscard]] auto HauntCount() const -> std::size_t { return haunts_.size(); }
  [[nodiscard]] auto Haunts() const -> const std::vector<Haunt>& { return haunts_; }
  [[nodiscard]] auto Haunts() -> std::vector<Haunt>& { return haunts_; }

  // Passive: CellApp iterates Haunts() and ships the builder output.
  // Source is owner's ReplicationState, so Ghost peers see the same
  // "other" projection the client would.
  [[nodiscard]] auto BuildPositionUpdate() const -> cellapp::GhostPositionUpdate;
  [[nodiscard]] auto BuildDelta() const -> cellapp::GhostDelta;
  [[nodiscard]] auto BuildSnapshotRefresh() const -> cellapp::GhostSnapshotRefresh;

  // True when the seq gap > 1 (BuildDelta would drop intermediates =>
  // use SnapshotRefresh). False for gap <= 1.
  [[nodiscard]] static constexpr auto ShouldUseSnapshotRefresh(uint64_t latest_event_seq,
                                                               uint64_t last_broadcast_event_seq)
      -> bool {
    return latest_event_seq > last_broadcast_event_seq + 1;
  }

  // All-zero => no audience-visible field update (only owner-visible
  // properties dirtied); skip SendMessage. Real still advances
  // last_broadcast_event_seq_ so next-tick gap arithmetic stays correct.
  [[nodiscard]] static auto IsEmptyOtherDelta(std::span<const std::byte> delta) -> bool {
    return std::all_of(delta.begin(), delta.end(), [](std::byte b) { return b == std::byte{0}; });
  }

  // CellApp updates last-sent values only after SendMessage succeeds;
  // this module stays agnostic to network errors.
  [[nodiscard]] auto LastBroadcastEventSeq() const -> uint64_t { return last_broadcast_event_seq_; }
  [[nodiscard]] auto LastBroadcastVolatileSeq() const -> uint64_t {
    return last_broadcast_volatile_seq_;
  }
  void MarkBroadcastEventSeq(uint64_t seq) { last_broadcast_event_seq_ = seq; }
  void MarkBroadcastVolatileSeq(uint64_t seq) { last_broadcast_volatile_seq_ = seq; }

  // Throttles ghost-pump fan-out to the update interval so per-tick
  // position changes don't fan out to every Haunt every tick.
  [[nodiscard]] auto LastBroadcastTime() const -> TimePoint { return last_broadcast_time_; }
  void MarkBroadcastTime(TimePoint t) { last_broadcast_time_ = t; }

  // OffloadChecker projects border crossings; call once per tick.
  [[nodiscard]] auto Velocity() const -> const math::Vector3& { return velocity_; }
  void UpdateVelocity(const math::Vector3& new_pos, float dt);

 private:
  CellEntity& owner_;
  std::vector<Haunt> haunts_;
  math::Vector3 velocity_{};
  math::Vector3 position_sample_{};
  bool has_position_sample_{false};

  // Broadcast-confirmed Real-side seqs; drives skip / delta / snapshot
  // decision in the ghost pump.
  uint64_t last_broadcast_event_seq_{0};
  uint64_t last_broadcast_volatile_seq_{0};
  TimePoint last_broadcast_time_{};
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_REAL_ENTITY_DATA_H_
