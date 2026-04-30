#ifndef ATLAS_SERVER_CELLAPP_CELL_ENTITY_H_
#define ATLAS_SERVER_CELLAPP_CELL_ENTITY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "math/vector3.h"
#include "network/address.h"
#include "server/entity_types.h"
#include "space/controllers.h"
#include "space/entity_motion.h"
#include "space/entity_range_list_node.h"

namespace atlas {

class Channel;
class RealEntityData;
class Space;
class Witness;

// Server-side cell entity; thin C++ shell over a C# script. Position/dir/
// on_ground live here so RangeList shuffles avoid the interop boundary.
// Destruction is reverse-declaration: witness -> controllers -> range_node_;
// witness must clear before range_node_ unlinks.
class CellEntity : public IEntityMotion {
 public:
  // Disambiguates Ghost ctor; no implicit-conversion risk.
  struct GhostTag {};

  // Real ctor; allocates RealEntityData so IsReal() holds immediately.
  CellEntity(EntityID id, uint16_t type_id, Space& space, const math::Vector3& position,
             const math::Vector3& direction);

  // Ghost ctor; real_channel is non-owning, must outlive the ghost (or
  // be cleared by ConvertGhostToReal).
  CellEntity(GhostTag, EntityID id, uint16_t type_id, Space& space, const math::Vector3& position,
             const math::Vector3& direction, Channel* real_channel);

  ~CellEntity() override;

  CellEntity(const CellEntity&) = delete;
  auto operator=(const CellEntity&) -> CellEntity& = delete;

  [[nodiscard]] auto Id() const -> EntityID { return id_; }
  [[nodiscard]] auto TypeId() const -> uint16_t { return type_id_; }
  [[nodiscard]] auto GetSpace() -> Space& { return space_; }
  [[nodiscard]] auto GetSpace() const -> const Space& { return space_; }

  // Real (real_data_ set) XOR Ghost (real_channel_ set). Ghosts are
  // passive - writes must log+skip (see cellapp_native_provider.cc).
  [[nodiscard]] auto IsReal() const -> bool { return real_data_ != nullptr; }
  [[nodiscard]] auto IsGhost() const -> bool { return real_channel_ != nullptr; }

  [[nodiscard]] auto GetRealData() -> RealEntityData* { return real_data_.get(); }
  [[nodiscard]] auto GetRealData() const -> const RealEntityData* { return real_data_.get(); }
  [[nodiscard]] auto GetRealChannel() const -> Channel* { return real_channel_; }

  [[nodiscard]] auto NextRealAddr() const -> const Address& { return next_real_addr_; }
  void SetNextRealAddr(const Address& addr) { next_real_addr_ = addr; }

  // Offload-emit side: drops RealEntityData (haunts leave with it),
  // parks real_channel, tears down witness + controllers (Ghost is
  // script-less).
  void ConvertRealToGhost(Channel* new_real_channel);

  // Offload-receive side: allocates fresh RealEntityData, retains
  // replication_state_ so we serve the cached snapshot until the next
  // C# frame publishes.
  void ConvertGhostToReal();

  // Ghost-only writes; no-op on a Real (would stomp authoritative state).
  void GhostUpdatePosition(const math::Vector3& position, const math::Vector3& direction,
                           bool on_ground, uint64_t volatile_seq);
  void GhostApplyDelta(uint64_t event_seq, std::span<const std::byte> other_delta);
  void GhostApplySnapshot(uint64_t event_seq, std::span<const std::byte> other_snapshot);

  // Used by GhostSetReal post-Offload; clears next_real_addr_. No-op
  // on a Real (no back-channel to rebind).
  void RebindRealChannel(Channel* new_real_channel);

  // Both setters update the field AND the RangeList sort position.
  [[nodiscard]] auto Position() const -> const math::Vector3& override { return position_; }
  [[nodiscard]] auto Direction() const -> const math::Vector3& override { return direction_; }
  void SetPosition(const math::Vector3& pos) override;
  void SetDirection(const math::Vector3& dir) override;

  // Saves one RangeList shuffle when caller has both.
  void SetPositionAndDirection(const math::Vector3& pos, const math::Vector3& dir);

  [[nodiscard]] auto OnGround() const -> bool { return on_ground_; }
  void SetOnGround(bool v) { on_ground_ = v; }

  [[nodiscard]] auto ScriptHandle() const -> uint64_t { return script_handle_; }
  void SetScriptHandle(uint64_t h) { script_handle_ = h; }

  [[nodiscard]] auto BaseAddr() const -> const Address& { return base_addr_; }
  void SetBaseAddr(const Address& addr) { base_addr_ = addr; }

  // Client-bound entities attach a Witness via EnableWitness; server-only
  // NPCs leave it unset. Dtor tears it down before range_node_ unlinks.
  [[nodiscard]] auto HasWitness() const -> bool { return witness_ != nullptr; }
  [[nodiscard]] auto GetWitness() -> Witness* { return witness_.get(); }
  [[nodiscard]] auto GetWitness() const -> const Witness* { return witness_.get(); }

  // Replaces any existing witness; activates immediately. send_unreliable
  // is optional (volatile path falls back to reliable for tests).
  // hysteresis=0 for strict single-band; >0 for dual-band (enter at
  // aoi_radius, leave at aoi_radius + hysteresis).
  template <typename ReliableFn>
  void EnableWitness(float aoi_radius, ReliableFn&& send_reliable, float hysteresis = 5.0f);
  template <typename ReliableFn, typename UnreliableFn>
  void EnableWitness(float aoi_radius, ReliableFn&& send_reliable, UnreliableFn&& send_unreliable,
                     float hysteresis = 5.0f);
  void DisableWitness();

  // Reverse AoI index for O(W) fan-out (W = observers).
  void AddObserver(Witness* w) { observers_.insert(w); }
  void RemoveObserver(Witness* w) { observers_.erase(w); }
  [[nodiscard]] auto Observers() const -> const std::unordered_set<Witness*>& { return observers_; }

  [[nodiscard]] auto GetControllers() -> Controllers& { return controllers_; }

  [[nodiscard]] auto RangeNode() -> EntityRangeListNode& { return range_node_; }

  // event_seq: ordered/cumulative property changes (Witness replays
  // history or falls back to snapshot). volatile_seq: latest-wins
  // pos/dir. event_seq == volatile_seq == 0 => no-op frame.
  struct ReplicationFrame {
    uint64_t event_seq{0};
    uint64_t volatile_seq{0};
    std::vector<std::byte> owner_delta;
    std::vector<std::byte> other_delta;
    math::Vector3 position{0.f, 0.f, 0.f};
    math::Vector3 direction{1.f, 0.f, 0.f};
    bool on_ground{false};

    // First Witness builds the envelope; siblings reuse -> eliminates
    // NxM serialisation. mutable: populated through const access.
    mutable std::vector<std::byte> cached_other_envelope;
  };

  struct ReplicationState {
    uint64_t latest_event_seq{0};
    uint64_t latest_volatile_seq{0};
    std::vector<std::byte> owner_snapshot;
    std::vector<std::byte> other_snapshot;
    std::deque<ReplicationFrame> history;
  };

  // ~800 ms at 10 Hz; larger values cost memory per entity.
  static constexpr std::size_t kReplicationHistoryWindow = 8;

  // event_seq advance: replace snapshots, push frame, pop oldest if full.
  // volatile_seq advance: adopt frame pos/dir/on_ground.
  // both zero: no-op. Snapshots only consumed on event_seq advance.
  void PublishReplicationFrame(ReplicationFrame frame, std::span<const std::byte> owner_snapshot,
                               std::span<const std::byte> other_snapshot);

  [[nodiscard]] auto GetReplicationState() const -> const ReplicationState*;

  // Test-only: synthesise pathological states. Asserts state is materialised.
  [[nodiscard]] auto GetReplicationStateMutableForTest() -> ReplicationState& {
    return *replication_state_;
  }

  [[nodiscard]] auto IsDestroyed() const -> bool { return destroyed_; }

  // Idempotent; tears range_node_ out and stops controllers. Object
  // deletion happens when Space erases the map entry.
  void Destroy();

 private:
  EntityID id_;
  uint16_t type_id_;
  math::Vector3 position_;
  math::Vector3 direction_;
  bool on_ground_{false};

  Space& space_;
  uint64_t script_handle_{0};

  Address base_addr_{};

  // Order matters: dtor runs reverse-declaration. range_node_ MUST unlink
  // last, after witness_/controllers_ remove their own trigger bounds.
  std::unique_ptr<Witness> witness_;
  Controllers controllers_;
  EntityRangeListNode range_node_;

  // Drained by the synthetic-shuffle Leave path before range_node_ teardown.
  std::unordered_set<Witness*> observers_;

  std::optional<ReplicationState> replication_state_;

  // Mutually exclusive: real_data_ XOR real_channel_.
  // next_real_addr_ is the in-flight Offload hint between
  // GhostSetNextReal and GhostSetReal.
  std::unique_ptr<RealEntityData> real_data_;
  Channel* real_channel_{nullptr};
  Address next_real_addr_{};

  bool destroyed_{false};
  bool linked_to_range_list_{false};
};

}  // namespace atlas

// Templates at tail: forward-declared Witness suffices in the class body,
// but instantiation needs the full type.

#include "foundation/log.h"
#include "witness.h"

namespace atlas {

template <typename ReliableFn>
void CellEntity::EnableWitness(float aoi_radius, ReliableFn&& send_reliable, float hysteresis) {
  // Witness only exists on a Real (it observes for a client-bound script).
  if (!IsReal()) {
    ATLAS_LOG_WARNING("CellEntity::EnableWitness on non-Real entity id={} — ignored", id_);
    return;
  }
  witness_ = std::make_unique<Witness>(*this, aoi_radius, hysteresis,
                                       std::forward<ReliableFn>(send_reliable));
  witness_->Activate();
}

template <typename ReliableFn, typename UnreliableFn>
void CellEntity::EnableWitness(float aoi_radius, ReliableFn&& send_reliable,
                               UnreliableFn&& send_unreliable, float hysteresis) {
  if (!IsReal()) {
    ATLAS_LOG_WARNING("CellEntity::EnableWitness on non-Real entity id={} — ignored", id_);
    return;
  }
  witness_ = std::make_unique<Witness>(*this, aoi_radius, hysteresis,
                                       std::forward<ReliableFn>(send_reliable),
                                       std::forward<UnreliableFn>(send_unreliable));
  witness_->Activate();
}

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELL_ENTITY_H_
