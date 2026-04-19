// RangeTrigger 2D detection tests — Phase 10 Step 10.2.
//
// The trigger reduces 2-D region membership to two 1-D linked-list
// shuffles. Tests cover:
//   1) peer enters / leaves a stationary trigger by moving itself,
//      including diagonal crossings
//   2) trigger moves (Update) and detects peers that are now in/out,
//      including the "expand first, contract second" ordering for
//      same-axis motion
//   3) SetRange expands/contracts in place
//   4) events fire ONLY for peers inside the orthogonal axis extent — a
//      peer that only crosses one boundary while outside the other is not
//      a 2-D member transition
//   5) ProximityController (the Controller-wrapped trigger) behaves
//      identically to the bare trigger

#include <memory>
#include <unordered_set>

#include <gtest/gtest.h>

#include "space/controllers.h"
#include "space/entity_range_list_node.h"
#include "space/proximity_controller.h"
#include "space/range_list.h"
#include "space/range_trigger.h"

namespace atlas {
namespace {

// Minimal test trigger that records which peers are inside.
class RecordingTrigger final : public RangeTrigger {
 public:
  RecordingTrigger(RangeListNode& central, float range) : RangeTrigger(central, range) {}

  void OnEnter(RangeListNode& other) override {
    inside_.insert(&other);
    ++enter_count_;
    last_entered_ = &other;
  }
  void OnLeave(RangeListNode& other) override {
    inside_.erase(&other);
    ++leave_count_;
    last_left_ = &other;
  }

  [[nodiscard]] auto IsInside(RangeListNode* node) const -> bool { return inside_.count(node) > 0; }
  [[nodiscard]] auto EnterCount() const -> int { return enter_count_; }
  [[nodiscard]] auto LeaveCount() const -> int { return leave_count_; }

  void ResetCounts() {
    enter_count_ = 0;
    leave_count_ = 0;
    last_entered_ = last_left_ = nullptr;
  }

  RangeListNode* last_entered_{nullptr};
  RangeListNode* last_left_{nullptr};

 private:
  std::unordered_set<RangeListNode*> inside_;
  int enter_count_{0};
  int leave_count_{0};
};

// ============================================================================
// Peer movement — basic 2-D semantics
// ============================================================================

TEST(RangeTrigger, PeerEntersOnBothAxesFiresOneOnEnter) {
  RangeList list;
  // Central at origin, range 5. Box is (-5,-5)..(+5,+5).
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());

  RecordingTrigger trig(*central, 5.f);
  trig.Insert(list);

  // Peer starts at (10, 10) — outside on both axes.
  auto peer = std::make_unique<EntityRangeListNode>(10.f, 10.f);
  list.Insert(peer.get());
  ASSERT_EQ(trig.EnterCount(), 0);

  // Move peer into box at (2, 2).
  peer->SetXZ(2.f, 2.f);
  list.ShuffleXThenZ(peer.get(), /*old_x=*/10.f, /*old_z=*/10.f);

  EXPECT_EQ(trig.EnterCount(), 1);
  EXPECT_EQ(trig.LeaveCount(), 0);
  EXPECT_TRUE(trig.IsInside(peer.get()));
  EXPECT_EQ(trig.last_entered_, peer.get());
}

TEST(RangeTrigger, PeerLeavesFiresOneOnLeave) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());

  // Create trigger BEFORE inserting peer at the inside position so we
  // avoid the Insert-pass enter.
  RecordingTrigger trig(*central, 5.f);
  trig.Insert(list);

  auto peer = std::make_unique<EntityRangeListNode>(2.f, 2.f);
  list.Insert(peer.get());
  // Insert of the peer itself fires one enter because peer crosses from
  // outside (sentinel left) into the box as it bubbles into place.
  ASSERT_EQ(trig.EnterCount(), 1);
  ASSERT_TRUE(trig.IsInside(peer.get()));
  trig.ResetCounts();

  // Move peer out.
  peer->SetXZ(20.f, 2.f);
  list.ShuffleXThenZ(peer.get(), 2.f, 2.f);

  EXPECT_EQ(trig.EnterCount(), 0);
  EXPECT_EQ(trig.LeaveCount(), 1);
  EXPECT_FALSE(trig.IsInside(peer.get()));
}

TEST(RangeTrigger, PeerCrossingOnlyXWhileZOutsideDoesNotFire) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());
  RecordingTrigger trig(*central, 5.f);
  trig.Insert(list);

  // Peer at (10, 20). Z is far above upper_z=5, so it's Z-outside.
  auto peer = std::make_unique<EntityRangeListNode>(10.f, 20.f);
  list.Insert(peer.get());
  trig.ResetCounts();

  // Move peer's X into the box's X range but keep Z outside.
  peer->SetXZ(0.f, 20.f);
  list.ShuffleXThenZ(peer.get(), 10.f, 20.f);

  // X axis transition happened but Z-outside → no 2-D event.
  EXPECT_EQ(trig.EnterCount(), 0);
  EXPECT_EQ(trig.LeaveCount(), 0);
  EXPECT_FALSE(trig.IsInside(peer.get()));
}

TEST(RangeTrigger, PeerDiagonalEntryFiresExactlyOnce) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());
  RecordingTrigger trig(*central, 5.f);
  trig.Insert(list);

  // Peer outside on BOTH axes.
  auto peer = std::make_unique<EntityRangeListNode>(10.f, 10.f);
  list.Insert(peer.get());
  trig.ResetCounts();

  // Move diagonally into box.
  peer->SetXZ(1.f, 1.f);
  list.ShuffleXThenZ(peer.get(), 10.f, 10.f);

  EXPECT_EQ(trig.EnterCount(), 1) << "diagonal entry must collapse to a single OnEnter";
  EXPECT_EQ(trig.LeaveCount(), 0);
}

// ============================================================================
// Central movement — RangeTrigger::Update
// ============================================================================

TEST(RangeTrigger, CentralMoveSweepsUpStationaryPeer) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());
  RecordingTrigger trig(*central, 5.f);
  trig.Insert(list);

  // Peer fixed at (20, 0) — outside X range of (-5,+5).
  auto peer = std::make_unique<EntityRangeListNode>(20.f, 0.f);
  list.Insert(peer.get());
  trig.ResetCounts();

  // Move central so the peer falls inside: central to (18, 0) ⇒ box is
  // (13,-5)..(23,+5), peer.x=20 is inside, peer.z=0 is inside.
  const float old_x = central->X();
  const float old_z = central->Z();
  central->SetXZ(18.f, 0.f);
  list.ShuffleXThenZ(central.get(), old_x, old_z);
  trig.Update(old_x, old_z);

  EXPECT_EQ(trig.EnterCount(), 1);
  EXPECT_TRUE(trig.IsInside(peer.get()));
}

TEST(RangeTrigger, CentralMoveAwayLeavesPeer) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());
  RecordingTrigger trig(*central, 5.f);
  trig.Insert(list);

  // Peer at (2, 2) — inside.
  auto peer = std::make_unique<EntityRangeListNode>(2.f, 2.f);
  list.Insert(peer.get());
  ASSERT_TRUE(trig.IsInside(peer.get()));
  trig.ResetCounts();

  // Move central far away.
  central->SetXZ(100.f, 0.f);
  list.ShuffleXThenZ(central.get(), 0.f, 0.f);
  trig.Update(0.f, 0.f);

  EXPECT_EQ(trig.LeaveCount(), 1);
  EXPECT_FALSE(trig.IsInside(peer.get()));
}

// ============================================================================
// SetRange in place
// ============================================================================

TEST(RangeTrigger, SetRangeExpandSweepsInPeer) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());
  RecordingTrigger trig(*central, 5.f);
  trig.Insert(list);

  // Peer at (10, 0) — outside the initial r=5 box.
  auto peer = std::make_unique<EntityRangeListNode>(10.f, 0.f);
  list.Insert(peer.get());
  trig.ResetCounts();

  // Expand range to 15; peer now inside.
  trig.SetRange(15.f);

  EXPECT_EQ(trig.EnterCount(), 1);
  EXPECT_TRUE(trig.IsInside(peer.get()));
}

TEST(RangeTrigger, SetRangeContractKicksOutPeer) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());
  RecordingTrigger trig(*central, 15.f);
  trig.Insert(list);

  auto peer = std::make_unique<EntityRangeListNode>(10.f, 0.f);
  list.Insert(peer.get());
  ASSERT_TRUE(trig.IsInside(peer.get()));
  trig.ResetCounts();

  trig.SetRange(5.f);

  EXPECT_EQ(trig.LeaveCount(), 1);
  EXPECT_FALSE(trig.IsInside(peer.get()));
}

// ============================================================================
// ProximityController wrapping RangeTrigger
// ============================================================================

TEST(ProximityController, FiresEnterAndLeaveThroughController) {
  RangeList list;
  auto central = std::make_unique<EntityRangeListNode>(0.f, 0.f);
  list.Insert(central.get());

  int enter_count = 0;
  int leave_count = 0;
  Controllers ctrls;
  auto proximity = std::make_unique<ProximityController>(
      *central, list, /*range=*/5.f,
      [&enter_count](ProximityController&, RangeListNode&) { ++enter_count; },
      [&leave_count](ProximityController&, RangeListNode&) { ++leave_count; });
  auto id = ctrls.Add(std::move(proximity), /*motion=*/nullptr, 0);

  // Peer enters.
  auto peer = std::make_unique<EntityRangeListNode>(2.f, 2.f);
  list.Insert(peer.get());
  EXPECT_EQ(enter_count, 1);

  // Update is a no-op for proximity — ticking doesn't fire anything.
  ctrls.Update(0.5f);
  EXPECT_EQ(enter_count, 1);
  EXPECT_EQ(leave_count, 0);

  // Peer leaves.
  peer->SetXZ(100.f, 100.f);
  list.ShuffleXThenZ(peer.get(), 2.f, 2.f);
  EXPECT_EQ(leave_count, 1);

  // Cancelling the controller removes the trigger — subsequent peer
  // movements don't fire any more events.
  ctrls.Cancel(id);
  peer->SetXZ(1.f, 1.f);
  list.ShuffleXThenZ(peer.get(), 100.f, 100.f);
  EXPECT_EQ(enter_count, 1);  // unchanged
}

}  // namespace
}  // namespace atlas
