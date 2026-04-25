// RangeList unit tests.
//
// The index is the core of CellApp's spatial query path, so tests cover:
//   1) insert produces a correctly sorted list on both axes
//   2) shuffle after movement preserves sort on both axes
//   3) ties in coordinate are broken by RangeListOrder (the sort key that
//      makes RangeTrigger's rectangular membership semantics correct)
//   4) random workloads never corrupt the linked-list invariants
//   5) selective cross-notification fires only when flag predicates match
//   6) 1000-node shuffle pass stays cheap (<1ms on dev hardware) — the
//      algorithm is only useful if its per-tick amortised cost is sub-ms

#include <chrono>
#include <cstdint>
#include <deque>
#include <random>

#include <gtest/gtest.h>

#include "space/range_list.h"

namespace atlas {
namespace {

// A settable test node — real entities will have a dedicated subclass but
// the core algorithm is fully exercised by this stub.
class MovableNode : public RangeListNode {
 public:
  MovableNode(float x, float z, RangeListOrder order, RangeListFlags wants = RangeListFlags::kNone,
              RangeListFlags makes = RangeListFlags::kNone) {
    x_ = x;
    z_ = z;
    order_ = order;
    wants_flags_ = wants;
    makes_flags_ = makes;
  }

  [[nodiscard]] auto X() const -> float override { return x_; }
  [[nodiscard]] auto Z() const -> float override { return z_; }
  [[nodiscard]] auto Order() const -> RangeListOrder override { return order_; }

  void SetPos(float x, float z) {
    x_ = x;
    z_ = z;
  }

  // Counters so tests can verify notification fired (and how).
  int x_crossed_count{0};
  int z_crossed_count{0};

 protected:
  void OnCrossedX(RangeListNode& other, bool positive, float other_ortho) override {
    ++x_crossed_count;
    last_x_crossed_node_ = &other;
    last_x_crossed_positive_ = positive;
    last_x_crossed_other_ortho_ = other_ortho;
  }
  void OnCrossedZ(RangeListNode& other, bool positive, float other_ortho) override {
    ++z_crossed_count;
    last_z_crossed_node_ = &other;
    last_z_crossed_positive_ = positive;
    last_z_crossed_other_ortho_ = other_ortho;
  }

 public:
  RangeListNode* last_x_crossed_node_{nullptr};
  RangeListNode* last_z_crossed_node_{nullptr};
  bool last_x_crossed_positive_{false};
  bool last_z_crossed_positive_{false};
  float last_x_crossed_other_ortho_{0.f};
  float last_z_crossed_other_ortho_{0.f};

 private:
  float x_{0.f};
  float z_{0.f};
  RangeListOrder order_{RangeListOrder::kEntity};
};

// Assert that walking prev_x -> next_x from head to tail yields a
// monotonically non-decreasing x sequence, with ties resolved by Order.
auto ValidateXSort(RangeList& list) -> ::testing::AssertionResult {
  auto* n = list.Head().next_x_;
  auto* prev = &list.Head();
  while (n != nullptr && n != &list.Tail()) {
    if (prev->X() > n->X() || (prev->X() == n->X() && static_cast<uint16_t>(prev->Order()) >
                                                          static_cast<uint16_t>(n->Order()))) {
      return ::testing::AssertionFailure()
             << "x-sort violated: prev.x=" << prev->X() << " n.x=" << n->X();
    }
    prev = n;
    n = n->next_x_;
  }
  return ::testing::AssertionSuccess();
}

auto ValidateZSort(RangeList& list) -> ::testing::AssertionResult {
  auto* n = list.Head().next_z_;
  auto* prev = &list.Head();
  while (n != nullptr && n != &list.Tail()) {
    if (prev->Z() > n->Z() || (prev->Z() == n->Z() && static_cast<uint16_t>(prev->Order()) >
                                                          static_cast<uint16_t>(n->Order()))) {
      return ::testing::AssertionFailure()
             << "z-sort violated: prev.z=" << prev->Z() << " n.z=" << n->Z();
    }
    prev = n;
    n = n->next_z_;
  }
  return ::testing::AssertionSuccess();
}

// ============================================================================
// Insert + remove
// ============================================================================

TEST(RangeList, EmptyListHeadLinksDirectlyToTail) {
  RangeList list;
  EXPECT_EQ(list.Head().next_x_, &list.Tail());
  EXPECT_EQ(list.Head().next_z_, &list.Tail());
  EXPECT_EQ(list.Tail().prev_x_, &list.Head());
  EXPECT_EQ(list.Tail().prev_z_, &list.Head());
}

TEST(RangeList, InsertSortsOnBothAxes) {
  RangeList list;
  MovableNode a(5.f, 3.f, RangeListOrder::kEntity);
  MovableNode b(2.f, 7.f, RangeListOrder::kEntity);
  MovableNode c(8.f, 1.f, RangeListOrder::kEntity);

  list.Insert(&a);
  list.Insert(&b);
  list.Insert(&c);

  EXPECT_TRUE(ValidateXSort(list));
  EXPECT_TRUE(ValidateZSort(list));
}

TEST(RangeList, RemoveClearsPointersAndKeepsListValid) {
  RangeList list;
  MovableNode a(1.f, 1.f, RangeListOrder::kEntity);
  MovableNode b(2.f, 2.f, RangeListOrder::kEntity);

  list.Insert(&a);
  list.Insert(&b);
  list.Remove(&a);

  EXPECT_EQ(a.prev_x_, nullptr);
  EXPECT_EQ(a.next_x_, nullptr);
  EXPECT_EQ(a.prev_z_, nullptr);
  EXPECT_EQ(a.next_z_, nullptr);
  EXPECT_TRUE(ValidateXSort(list));
  EXPECT_TRUE(ValidateZSort(list));
}

// ============================================================================
// Shuffle after movement
// ============================================================================

TEST(RangeList, ShuffleAfterMoveMaintainsSortInvariant) {
  RangeList list;
  MovableNode a(5.f, 5.f, RangeListOrder::kEntity);
  MovableNode b(10.f, 10.f, RangeListOrder::kEntity);
  MovableNode c(15.f, 15.f, RangeListOrder::kEntity);

  list.Insert(&a);
  list.Insert(&b);
  list.Insert(&c);

  // Move a across both b and c.
  const float old_x = a.X();
  const float old_z = a.Z();
  a.SetPos(20.f, 20.f);
  list.ShuffleXThenZ(&a, old_x, old_z);

  EXPECT_TRUE(ValidateXSort(list));
  EXPECT_TRUE(ValidateZSort(list));
}

TEST(RangeList, ZeroMoveShuffleIsNoop) {
  // Re-running ShuffleXThenZ without actually moving must not damage the
  // list; CellEntity callers sometimes shuffle unconditionally (e.g. after
  // clamping to terrain) and we can't afford spurious re-sorts.
  RangeList list;
  MovableNode a(1.f, 2.f, RangeListOrder::kEntity);
  MovableNode b(3.f, 4.f, RangeListOrder::kEntity);
  list.Insert(&a);
  list.Insert(&b);

  list.ShuffleXThenZ(&a, a.X(), a.Z());
  list.ShuffleXThenZ(&b, b.X(), b.Z());

  EXPECT_TRUE(ValidateXSort(list));
  EXPECT_TRUE(ValidateZSort(list));
  EXPECT_EQ(a.x_crossed_count, 0);
  EXPECT_EQ(b.x_crossed_count, 0);
}

// ============================================================================
// Tie-break on Order
// ============================================================================

TEST(RangeList, SameCoordinateSortsByOrder) {
  RangeList list;
  // Three nodes at identical (x,z). Expected x-order: Entity < LowerBound < UpperBound.
  MovableNode entity(5.f, 5.f, RangeListOrder::kEntity);
  MovableNode lower(5.f, 5.f, RangeListOrder::kLowerBound);
  MovableNode upper(5.f, 5.f, RangeListOrder::kUpperBound);

  // Insert in reverse to force the shuffle to reorder.
  list.Insert(&upper);
  list.Insert(&lower);
  list.Insert(&entity);

  auto* cur = list.Head().next_x_;
  ASSERT_EQ(cur, &entity);
  cur = cur->next_x_;
  ASSERT_EQ(cur, &lower);
  cur = cur->next_x_;
  ASSERT_EQ(cur, &upper);
  ASSERT_EQ(cur->next_x_, &list.Tail());
}

// ============================================================================
// Cross notification — selective (opt-in flags)
// ============================================================================

TEST(RangeList, CrossNotificationFiresWhenFlagsOptIn) {
  RangeList list;
  MovableNode a(0.f, 0.f, RangeListOrder::kEntity,
                /*wants=*/RangeListFlags::kEntityTrigger,
                /*makes=*/RangeListFlags::kEntityTrigger);
  MovableNode b(5.f, 0.f, RangeListOrder::kEntity,
                /*wants=*/RangeListFlags::kEntityTrigger,
                /*makes=*/RangeListFlags::kEntityTrigger);

  list.Insert(&a);
  list.Insert(&b);

  // The Insert path itself bubbles `b` right past `a`, which is a legitimate
  // cross event — entity-join fan-out is how triggers discover pre-existing
  // neighbours. For this test we care about crosses produced by explicit
  // movement, so reset counters to baseline after setup.
  a.x_crossed_count = 0;
  b.x_crossed_count = 0;

  // Move a past b on the x axis.
  a.SetPos(10.f, 0.f);
  list.ShuffleXThenZ(&a, 0.f, 0.f);

  EXPECT_EQ(a.x_crossed_count, 1);
  EXPECT_EQ(b.x_crossed_count, 1);
  // a moved RIGHT past b -> a saw positive, b saw negative.
  EXPECT_TRUE(a.last_x_crossed_positive_);
  EXPECT_FALSE(b.last_x_crossed_positive_);
  EXPECT_EQ(a.last_x_crossed_node_, &b);
  EXPECT_EQ(b.last_x_crossed_node_, &a);
}

TEST(RangeList, CrossNotificationSuppressedWhenFlagsDisjoint) {
  RangeList list;
  MovableNode a(0.f, 0.f, RangeListOrder::kEntity,
                /*wants=*/RangeListFlags::kEntityTrigger,
                /*makes=*/RangeListFlags::kEntityTrigger);
  // b wants / makes a DIFFERENT flag, so the cross should stay silent.
  MovableNode b(5.f, 0.f, RangeListOrder::kEntity,
                /*wants=*/RangeListFlags::kLowerAoiTrigger,
                /*makes=*/RangeListFlags::kLowerAoiTrigger);

  list.Insert(&a);
  list.Insert(&b);
  a.SetPos(10.f, 0.f);
  list.ShuffleXThenZ(&a, 0.f, 0.f);

  EXPECT_EQ(a.x_crossed_count, 0);
  EXPECT_EQ(b.x_crossed_count, 0);
}

// ============================================================================
// Random workload stays consistent
// ============================================================================

TEST(RangeList, RandomInsertAndMovePreservesInvariants) {
  RangeList list;
  // deque, not vector: MovableNode is non-copyable/non-movable (RangeListNode
  // deletes copy), so a vector can't reallocate. deque never relocates
  // existing elements on push_back, so addresses (which RangeList holds by
  // pointer) stay stable.
  std::deque<MovableNode> nodes;
  std::mt19937 rng(0xA71A5);  // deterministic seed for reproducible fail traces
  std::uniform_real_distribution<float> coord(-100.f, 100.f);

  // Bulk insert.
  for (int i = 0; i < 200; ++i) {
    nodes.emplace_back(coord(rng), coord(rng), RangeListOrder::kEntity);
    list.Insert(&nodes.back());
  }
  ASSERT_TRUE(ValidateXSort(list));
  ASSERT_TRUE(ValidateZSort(list));

  // Random tiny moves (simulates an AoI-heavy tick).
  std::uniform_real_distribution<float> jitter(-2.f, 2.f);
  for (int step = 0; step < 500; ++step) {
    auto& n = nodes[static_cast<size_t>(rng() % nodes.size())];
    const float old_x = n.X();
    const float old_z = n.Z();
    n.SetPos(old_x + jitter(rng), old_z + jitter(rng));
    list.ShuffleXThenZ(&n, old_x, old_z);
  }

  EXPECT_TRUE(ValidateXSort(list));
  EXPECT_TRUE(ValidateZSort(list));
}

// ============================================================================
// Performance — 1000 shuffles should be sub-millisecond budget on dev boxes.
// We keep the threshold loose (5ms) so CI variance doesn't flake the test;
// the real regression signal is in absolute trend tracking over time, not
// the specific number here.
// ============================================================================

TEST(RangeList, ShufflePerformanceBudget) {
  RangeList list;
  std::deque<MovableNode> nodes;  // stable addresses; see note in random test
  std::mt19937 rng(0xF00D);
  std::uniform_real_distribution<float> coord(-100.f, 100.f);
  for (int i = 0; i < 1000; ++i) {
    nodes.emplace_back(coord(rng), coord(rng), RangeListOrder::kEntity);
    list.Insert(&nodes.back());
  }

  std::uniform_real_distribution<float> jitter(-1.f, 1.f);
  auto start = std::chrono::steady_clock::now();
  for (auto& n : nodes) {
    const float old_x = n.X();
    const float old_z = n.Z();
    n.SetPos(old_x + jitter(rng), old_z + jitter(rng));
    list.ShuffleXThenZ(&n, old_x, old_z);
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

  EXPECT_LT(ms, 5) << "1000 small shuffles took " << ms << "ms (budget 5ms)";
  EXPECT_TRUE(ValidateXSort(list));
  EXPECT_TRUE(ValidateZSort(list));
}

}  // namespace
}  // namespace atlas
