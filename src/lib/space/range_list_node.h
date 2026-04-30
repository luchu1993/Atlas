#ifndef ATLAS_LIB_SPACE_RANGE_LIST_NODE_H_
#define ATLAS_LIB_SPACE_RANGE_LIST_NODE_H_

#include <cstdint>
#include <type_traits>

namespace atlas {

// RangeListOrder disambiguates "nodes at exactly the same coordinate".
// The numeric values encode the stable ordering
// required to keep the 2-D region semantics consistent:
//     Head  < ... < Entity < LowerBound < UpperBound < ... < Tail
// so that an entity sitting at x == trigger.upper_x is considered OUTSIDE
// the trigger (upper bound sorts after the entity).
enum class RangeListOrder : uint16_t {
  kHead = 0,          // -FLT_MAX sentinel (head)
  kEntity = 100,      // regular entity node
  kLowerBound = 190,  // trigger lower-bound node
  kUpperBound = 200,  // trigger upper-bound node
  kTail = 0xFFFF,     // +FLT_MAX sentinel (tail)
};

// Flag vocabulary for selective cross-notification. A cross-notification
// fires only when (a.makes & b.wants) | (b.makes & a.wants) is non-zero
// so both sides must opt in before we pay the virtual call.
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

  [[nodiscard]] virtual auto X() const -> float = 0;
  [[nodiscard]] virtual auto Z() const -> float = 0;
  [[nodiscard]] virtual auto Order() const -> RangeListOrder = 0;

  [[nodiscard]] auto WantsFlags() const -> RangeListFlags { return wants_flags_; }
  [[nodiscard]] auto MakesFlags() const -> RangeListFlags { return makes_flags_; }

  RangeListNode* prev_x_{nullptr};
  RangeListNode* next_x_{nullptr};
  RangeListNode* prev_z_{nullptr};
  RangeListNode* next_z_{nullptr};

 protected:
  // Subclasses override to react to axis crossings. The `positive` argument
  // is the direction of motion of `this` node relative to `other`: true when
  // `this` is now at a higher coordinate than `other`, false when lower.
  // `other_ortho` is the orthogonal-axis coordinate at the cross point.
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
