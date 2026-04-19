#ifndef ATLAS_LIB_SPACE_RANGE_LIST_NODE_H_
#define ATLAS_LIB_SPACE_RANGE_LIST_NODE_H_

#include <cstdint>
#include <type_traits>

namespace atlas {

// ============================================================================
// RangeList — a doubly-sorted linked list of world-space nodes
//
// This is the core spatial index for Atlas's single-CellApp AoI / trigger
// system. The design is a direct port of BigWorld's RangeList (15 years of
// MMO production) and exploits the fact that most entity movement is small
// and frame-to-frame shuffles bubble only a handful of hops on average:
//
//   - Each node lives in TWO linked lists simultaneously: one sorted by x,
//     one by z. The axes are decoupled on purpose; a 2D-distance check is
//     decomposed into two separate 1D shuffles + a cross-axis bound test.
//   - Fixed head/tail sentinels (at +/- FLT_MAX) make link insertion
//     branch-free — no "am I at the boundary?" special cases in the hot loop.
//   - When two nodes swap past each other during a shuffle, both are asked
//     to react via the virtual on_crossed_x / on_crossed_z callback. This is
//     the hook RangeTrigger uses to emit on_enter / on_leave events.
//
// RangeListOrder disambiguates "nodes at exactly the same coordinate".
// The numeric values aren't arbitrary — they encode the stable ordering
// required to keep the 2-D region semantics consistent:
//
//     Head  < ... < Entity < LowerBound < UpperBound < ... < Tail
//
// so that an entity sitting at x == trigger.upper_x is considered OUTSIDE
// the trigger (upper bound sorts after the entity).
// ============================================================================

enum class RangeListOrder : uint16_t {
  kHead = 0,          // -FLT_MAX sentinel (head)
  kEntity = 100,      // regular entity node
  kLowerBound = 190,  // trigger lower-bound node
  kUpperBound = 200,  // trigger upper-bound node
  kTail = 0xFFFF,     // +FLT_MAX sentinel (tail)
};

// Flag vocabulary for selective cross-notification. A cross-notification
// fires only when (a.makes & b.wants) | (b.makes & a.wants) is non-zero
// — i.e. both sides must opt in by flag before we pay the virtual call.
enum class RangeListFlags : uint8_t {
  kNone = 0,
  kEntityTrigger = 0x01,    // entity crossing a trigger (both sides)
  kLowerAoiTrigger = 0x02,  // AoI trigger's lower-bound node
  kUpperAoiTrigger = 0x04,  // AoI trigger's upper-bound node
  kIsEntity = 0x10,         // the node represents a CellEntity
  kIsLowerBound = 0x20,     // the node is a lower-bound trigger
};

constexpr auto operator|(RangeListFlags a, RangeListFlags b) -> RangeListFlags {
  using U = std::underlying_type_t<RangeListFlags>;
  return static_cast<RangeListFlags>(static_cast<U>(a) | static_cast<U>(b));
}

constexpr auto operator&(RangeListFlags a, RangeListFlags b) -> RangeListFlags {
  using U = std::underlying_type_t<RangeListFlags>;
  return static_cast<RangeListFlags>(static_cast<U>(a) & static_cast<U>(b));
}

constexpr auto operator|=(RangeListFlags& a, RangeListFlags b) -> RangeListFlags& {
  a = a | b;
  return a;
}

constexpr auto Any(RangeListFlags v) -> bool {
  return static_cast<std::underlying_type_t<RangeListFlags>>(v) != 0;
}

class RangeList;

class RangeListNode {
 public:
  RangeListNode() = default;
  virtual ~RangeListNode() = default;

  RangeListNode(const RangeListNode&) = delete;
  auto operator=(const RangeListNode&) -> RangeListNode& = delete;

  // The core query API — coordinates and tie-break order. Marked `volatile`
  // in intent (not syntax — C++ `volatile` would disable optimisations we
  // want) so shuffle passes must read them explicitly each iteration; that
  // guards against compilers caching a stale field across a crossed-callback
  // that itself writes the node's position.
  [[nodiscard]] virtual auto X() const -> float = 0;
  [[nodiscard]] virtual auto Z() const -> float = 0;
  [[nodiscard]] virtual auto Order() const -> RangeListOrder = 0;

  [[nodiscard]] auto WantsFlags() const -> RangeListFlags { return wants_flags_; }
  [[nodiscard]] auto MakesFlags() const -> RangeListFlags { return makes_flags_; }

  // X-axis neighbours.
  RangeListNode* prev_x_{nullptr};
  RangeListNode* next_x_{nullptr};
  // Z-axis neighbours.
  RangeListNode* prev_z_{nullptr};
  RangeListNode* next_z_{nullptr};

 protected:
  // Subclasses override to react to axis crossings. The `positive` argument
  // is the direction of motion of `this` node relative to `other`: true when
  // `this` is now at a higher coordinate than `other`, false when lower.
  //
  // `other_ortho` is the orthogonal-axis coordinate of `other` AT THE CROSS:
  //   - For OnCrossedX callbacks: `other`'s Z (either its current Z if
  //     `other` is stationary, or its OLD Z if `other` is the moving node —
  //     because X shuffles run before Z shuffles, the moving node's Z
  //     hasn't been updated yet when the X cross happens).
  //   - For OnCrossedZ callbacks: `other`'s X (already updated if `other`
  //     moved this tick, since X shuffles happen before Z).
  //
  // 2-D triggers (RangeTrigger) consume this to decide whether a single-axis
  // crossing maps to a 2-D enter/leave event. Nodes that only care about
  // 1-D ordering can ignore it.
  virtual void OnCrossedX(RangeListNode& other, bool positive, float other_ortho) {
    (void)other;
    (void)positive;
    (void)other_ortho;
  }
  virtual void OnCrossedZ(RangeListNode& other, bool positive, float other_ortho) {
    (void)other;
    (void)positive;
    (void)other_ortho;
  }

  RangeListFlags wants_flags_{RangeListFlags::kNone};
  RangeListFlags makes_flags_{RangeListFlags::kNone};

  friend class RangeList;
};

// Fixed-position node used for sentinels; exposed so tests can synthesise a
// RangeList without any live entities.
class FixedRangeListNode final : public RangeListNode {
 public:
  FixedRangeListNode(float x, float z, RangeListOrder order) : x_(x), z_(z), order_(order) {}

  [[nodiscard]] auto X() const -> float override { return x_; }
  [[nodiscard]] auto Z() const -> float override { return z_; }
  [[nodiscard]] auto Order() const -> RangeListOrder override { return order_; }

 private:
  float x_;
  float z_;
  RangeListOrder order_;
};

}  // namespace atlas

#endif  // ATLAS_LIB_SPACE_RANGE_LIST_NODE_H_
