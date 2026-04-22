#ifndef ATLAS_SERVER_CELLAPP_CELL_ENTITY_H_
#define ATLAS_SERVER_CELLAPP_CELL_ENTITY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
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

// ============================================================================
// CellEntity — server-side cell-layer entity
//
// CellEntity is deliberately a thin C++ shell (~few hundred LOC) on top of
// a C# script instance held by handle. Position/direction/on_ground live
// in C++ because the RangeList shuffle must read them without crossing
// the interop boundary; everything else — scripting, RPC dispatch, game
// logic — stays in C#. See phase10_cellapp.md §1.3.
//
// Lifecycle (see phase10_cellapp.md §3.10 #4):
//   1. Construct with (id, type_id, space, pos, dir). Ctor inserts the
//      range_node_ into space's RangeList.
//   2. Attach witness / controllers as game logic demands.
//   3. Per-tick:
//        - C# script mutates properties and position
//        - C# calls PublishReplicationFrame (via NativeApi) to hand the
//          Witness layer a per-tick delta/snapshot bundle
//   4. Destroy(): witness is torn down FIRST (so its AoITrigger bounds
//      come out of RangeList while central is still live), then
//      controllers, then the range_node_ itself.
// ============================================================================

class CellEntity : public IEntityMotion {
 public:
  // Tag used to disambiguate the Ghost ctor — no arg-count clash, no
  // accidental implicit conversion, and it makes call sites self-describing.
  struct GhostTag {};

  // Phase 10 ctor — creates a REAL entity. The RealEntityData sidecar is
  // allocated so is_real() returns true immediately.
  CellEntity(EntityID id, uint16_t type_id, Space& space, const math::Vector3& position,
             const math::Vector3& direction);

  // Phase 11 ctor — creates a GHOST replica. `real_channel` is the non-
  // owning back-channel to the Real CellApp; it must remain valid for
  // the lifetime of the ghost (cleared by ConvertGhostToReal on Offload
  // promotion, or cleared externally before the entity is destroyed).
  CellEntity(GhostTag, EntityID id, uint16_t type_id, Space& space, const math::Vector3& position,
             const math::Vector3& direction, Channel* real_channel);

  ~CellEntity() override;

  CellEntity(const CellEntity&) = delete;
  auto operator=(const CellEntity&) -> CellEntity& = delete;

  // ---- Identity -------------------------------------------------------------

  [[nodiscard]] auto Id() const -> EntityID { return id_; }
  [[nodiscard]] auto TypeId() const -> uint16_t { return type_id_; }
  [[nodiscard]] auto GetSpace() -> Space& { return space_; }
  [[nodiscard]] auto GetSpace() const -> const Space& { return space_; }

  // ---- Real / Ghost state ---------------------------------------------------
  //
  // An entity is exactly one of: Real (real_data_ set) or Ghost
  // (real_channel_ set). The pair is kept mutually exclusive by
  // Convert{Real,Ghost}To{Ghost,Real}. See Phase 11 §3.1.
  //
  // Ghosts are passive C++ data containers — no Witness, no Controllers,
  // no script instance. Every writeable CellApp entry point must
  // log-and-skip when it sees a Ghost (Q2 resolution: soft guard, never
  // hard assert); see cellapp_native_provider.cc for the pattern.

  [[nodiscard]] auto IsReal() const -> bool { return real_data_ != nullptr; }
  [[nodiscard]] auto IsGhost() const -> bool { return real_channel_ != nullptr; }

  [[nodiscard]] auto GetRealData() -> RealEntityData* { return real_data_.get(); }
  [[nodiscard]] auto GetRealData() const -> const RealEntityData* { return real_data_.get(); }
  [[nodiscard]] auto GetRealChannel() const -> Channel* { return real_channel_; }

  [[nodiscard]] auto NextRealAddr() const -> const Address& { return next_real_addr_; }
  void SetNextRealAddr(const Address& addr) { next_real_addr_ = addr; }

  // Real → Ghost transition, invoked by CellApp at the emitting end of an
  // Offload. Drops the RealEntityData sidecar (the haunt list leaves
  // with it) and parks `real_channel` as the new back-channel. The
  // witness is torn down because Ghost is script-less; controllers are
  // stopped for the same reason.
  void ConvertRealToGhost(Channel* new_real_channel);

  // Ghost → Real transition, invoked by CellApp at the receiving end of
  // an Offload. Clears the back-channel, allocates a fresh
  // RealEntityData. Existing replication_state_ is retained — the new
  // Real keeps serving the already-baselined snapshot until its C#
  // instance publishes the next frame.
  void ConvertGhostToReal();

  // Ghost-only writes. Each of these is a no-op on a Real entity
  // (log-and-return) because the Real's authoritative state would
  // otherwise be stomped by a stale cross-process delivery.

  void GhostUpdatePosition(const math::Vector3& position, const math::Vector3& direction,
                           bool on_ground, uint64_t volatile_seq);
  void GhostApplyDelta(uint64_t event_seq, std::span<const std::byte> other_delta);
  void GhostApplySnapshot(uint64_t event_seq, std::span<const std::byte> other_snapshot);

  // Rebinds the Ghost's back-channel. Used by CellApp::OnGhostSetReal after
  // another CellApp becomes the new Real following a successful Offload.
  // Also clears next_real_addr_ since the handoff is complete. No-op
  // (log-and-return) on a Real since Reals don't have a back-channel.
  void RebindRealChannel(Channel* new_real_channel);

  // ---- IEntityMotion --------------------------------------------------------
  //
  // SetPosition / SetDirection update both the in-memory field AND the
  // RangeList sort position — callers can blindly set and rely on spatial
  // queries seeing the new placement before returning.
  [[nodiscard]] auto Position() const -> const math::Vector3& override { return position_; }
  [[nodiscard]] auto Direction() const -> const math::Vector3& override { return direction_; }
  void SetPosition(const math::Vector3& pos) override;
  void SetDirection(const math::Vector3& dir) override;

  // Combined setter — saves one RangeList shuffle when caller has both.
  void SetPositionAndDirection(const math::Vector3& pos, const math::Vector3& dir);

  [[nodiscard]] auto OnGround() const -> bool { return on_ground_; }
  void SetOnGround(bool v) { on_ground_ = v; }

  // ---- Script handle --------------------------------------------------------

  [[nodiscard]] auto ScriptHandle() const -> uint64_t { return script_handle_; }
  void SetScriptHandle(uint64_t h) { script_handle_ = h; }

  // ---- Base mailbox ---------------------------------------------------------

  [[nodiscard]] auto BaseAddr() const -> const Address& { return base_addr_; }
  [[nodiscard]] auto BaseEntityId() const -> EntityID { return base_entity_id_; }
  void SetBase(const Address& addr, EntityID base_id) {
    base_addr_ = addr;
    base_entity_id_ = base_id;
  }

  // ---- Witness (AoI observer) ----------------------------------------------
  //
  // Entities that watch a client attach a Witness via EnableWitness.
  // Server-only NPCs normally leave the witness unset. The witness must
  // be torn down BEFORE the range_node_ unlinks — see phase10_cellapp.md
  // §3.10 #4 — so CellEntity's destructor handles that ordering.

  [[nodiscard]] auto HasWitness() const -> bool { return witness_ != nullptr; }
  [[nodiscard]] auto GetWitness() -> Witness* { return witness_.get(); }
  [[nodiscard]] auto GetWitness() const -> const Witness* { return witness_.get(); }

  // Install (replacing any existing) a witness with the given radius and
  // delivery callbacks. The witness activates immediately. The
  // `send_unreliable` callback is optional — when omitted, volatile-path
  // envelopes fall back to the reliable callback so tests can exercise
  // the state machine without wiring two transports.
  // `hysteresis` defaults to 5.0f — matches BigWorld's Witness::aoiHyst_
  // initial value (witness.cpp:136). Pass 0.f for strict single-band
  // trigger semantics; pass a positive value for the dual-band hysteresis
  // AoITrigger (enter at aoi_radius, leave at aoi_radius + hysteresis).
  template <typename ReliableFn>
  void EnableWitness(float aoi_radius, ReliableFn&& send_reliable, float hysteresis = 5.0f);
  template <typename ReliableFn, typename UnreliableFn>
  void EnableWitness(float aoi_radius, ReliableFn&& send_reliable, UnreliableFn&& send_unreliable,
                     float hysteresis = 5.0f);
  void DisableWitness();

  // ---- Controllers ----------------------------------------------------------

  [[nodiscard]] auto GetControllers() -> Controllers& { return controllers_; }

  // ---- RangeList ------------------------------------------------------------

  [[nodiscard]] auto RangeNode() -> EntityRangeListNode& { return range_node_; }

  // ---- Replication frame / state -------------------------------------------
  //
  // Two independent monotonic sequences drive CellApp's AoI replication:
  //
  //   event_seq  — property changes. Ordered, cumulative; a Witness that
  //                 falls behind either replays history or falls back to
  //                 the owner/other snapshot.
  //   volatile_seq — position/orientation. Latest-wins; a Witness only
  //                 ever cares about the most recent one.
  //
  // A ReplicationFrame captures one tick's worth of either or both. A
  // frame with event_seq == 0 AND volatile_seq == 0 is a no-op.

  struct ReplicationFrame {
    uint64_t event_seq{0};
    uint64_t volatile_seq{0};
    std::vector<std::byte> owner_delta;
    std::vector<std::byte> other_delta;
    math::Vector3 position{0.f, 0.f, 0.f};
    math::Vector3 direction{1.f, 0.f, 0.f};
    bool on_ground{false};
  };

  struct ReplicationState {
    uint64_t latest_event_seq{0};
    uint64_t latest_volatile_seq{0};
    std::vector<std::byte> owner_snapshot;  // replaced per property tick
    std::vector<std::byte> other_snapshot;
    std::deque<ReplicationFrame> history;  // bounded window
  };

  // Maximum frames kept in history. Chosen to cover the common latency
  // window at 10 Hz (~800 ms). Larger values cost memory per entity; see
  // phase10_cellapp.md §3.11 memory budget.
  static constexpr std::size_t kReplicationHistoryWindow = 8;

  // C# per-tick entry point (driven by NativeApi in Step 10.7):
  //   - event_seq > latest_event_seq:
  //       advance latest_event_seq; replace owner/other snapshots with
  //       the supplied buffers; append frame to history; pop oldest if
  //       window full.
  //   - volatile_seq > latest_volatile_seq:
  //       advance latest_volatile_seq; adopt frame.position/direction/
  //       on_ground as the new C++ mirror of position state.
  //   - both zero: no-op (the frame came from a tick with nothing to say).
  //
  // owner_snapshot / other_snapshot are only consumed when event_seq is
  // advancing; callers should pass empty spans otherwise.
  void PublishReplicationFrame(ReplicationFrame frame, std::span<const std::byte> owner_snapshot,
                               std::span<const std::byte> other_snapshot);

  [[nodiscard]] auto GetReplicationState() const -> const ReplicationState*;

  // ---- Destruction ----------------------------------------------------------

  [[nodiscard]] auto IsDestroyed() const -> bool { return destroyed_; }

  // Idempotent. Callers mark the entity dead; Space removes it on the
  // next compaction pass. Destroy tears the range_node_ out of the list
  // and stops all controllers; the actual object deletion happens when
  // Space's map entry is erased.
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
  EntityID base_entity_id_{kInvalidEntityID};

  // Field order matters: witness_ before controllers_ before range_node_
  // so the destructor (which runs in REVERSE declaration order) tears
  // them down in the correct sequence: range_node_ unlinks LAST, after
  // witness_ and controllers_ have had a chance to remove their own
  // trigger bounds. See phase10_cellapp.md §3.10 #4.
  std::unique_ptr<Witness> witness_;
  Controllers controllers_;
  EntityRangeListNode range_node_;

  std::optional<ReplicationState> replication_state_;

  // Phase 11 Real/Ghost state (mutually exclusive):
  //   real_data_    non-null → Real
  //   real_channel_ non-null → Ghost
  // next_real_addr_ is the Offload-transition hint a Ghost carries while
  // its old Real is mid-handoff; it's only meaningful between the old
  // Real's GhostSetNextReal and the new Real's GhostSetReal.
  std::unique_ptr<RealEntityData> real_data_;
  Channel* real_channel_{nullptr};
  Address next_real_addr_{};

  bool destroyed_{false};
  bool linked_to_range_list_{false};
};

}  // namespace atlas

// ---- Template definitions ---------------------------------------------------
// Kept at header-file tail so the Witness forward declaration is enough for
// the class body while full Witness visibility is required for the template
// instantiation site (implicit inline).

#include "foundation/log.h"
#include "witness.h"

namespace atlas {

template <typename ReliableFn>
void CellEntity::EnableWitness(float aoi_radius, ReliableFn&& send_reliable, float hysteresis) {
  // Ghost rejection (Phase 11 §3.1): a Witness observes peer AoI on behalf
  // of a client-bound script, which only exists on a Real. Silently
  // attaching one to a Ghost would leak events out through a channel
  // that doesn't own the entity.
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
