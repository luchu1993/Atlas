// Lifetime edges on RangeTrigger — review round 2.
//
// RangeTrigger::Remove() is the only documented way to unlink a
// trigger's bound nodes from a RangeList. But a trigger whose owner
// drops the unique_ptr WITHOUT calling Remove — e.g. `Witness` replaced
// via `CellEntity::EnableWitness(...)` twice — would previously leave
// dangling bound nodes in the list pointing at a destroyed trigger.
// The next shuffle that crosses them would vcall OnCrossedX/Z on freed
// memory. This test exercises the replacement path.

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "space/range_trigger.h"
#include "witness.h"

namespace atlas {
namespace {

// Shared witness delivery that just counts envelopes — the test cares
// about "did the list stay structurally consistent" rather than envelope
// content.
class RangeTriggerLifetimeTest : public ::testing::Test {
 protected:
  Space space_{1};

  auto MakeEntity(EntityID id, math::Vector3 pos = {0, 0, 0}) -> CellEntity* {
    auto* e = space_.AddEntity(
        std::make_unique<CellEntity>(id, uint16_t{1}, space_, pos, math::Vector3{1, 0, 0}));
    e->SetBaseAddr(Address(0, 0));
    return e;
  }
};

// Drop the old witness via reassign — the NEW witness's trigger must
// work correctly (meaning peer crossings fire on IT, not the dead one).
// If the old bounds lingered in the RangeList, ShuffleXThenZ would call
// OnCrossedX on dead memory and either crash (ASan) or dispatch into
// a destroyed Witness's HandleAoILeave.
TEST_F(RangeTriggerLifetimeTest, EnableWitnessTwiceReleasesOldTriggerBounds) {
  auto* observer = MakeEntity(1, {0, 0, 0});

  int enters_on_first = 0;
  observer->EnableWitness(5.f, [&](EntityID, std::span<const std::byte> env) {
    if (!env.empty() && static_cast<uint8_t>(env[0]) == 1) ++enters_on_first;
  });

  // Replace the witness. The old Witness's unique_ptr drops → the old
  // AoITrigger destroys. Without a safety net in ~RangeTrigger, its
  // lower/upper bound nodes stay in the RangeList as zombies.
  int enters_on_second = 0;
  observer->EnableWitness(5.f, [&](EntityID, std::span<const std::byte> env) {
    if (!env.empty() && static_cast<uint8_t>(env[0]) == 1) ++enters_on_second;
  });

  // Spawn a peer inside the AoI. The NEW witness must fire EntityEnter;
  // no zombie callback from the dead witness must fire.
  auto* peer = MakeEntity(2, {2, 0, 2});
  observer->GetWitness()->Update(4096);

  EXPECT_EQ(enters_on_first, 0) << "old witness should not see any events";
  EXPECT_EQ(enters_on_second, 1) << "new witness should receive exactly one enter";
  EXPECT_EQ(observer->GetWitness()->AoIMap().size(), 1u);
  (void)peer;
}

// Direct test of RangeTrigger's own cleanup contract: dropping a
// trigger whose bounds are still linked should unlink them.
struct NoOpTrigger final : public RangeTrigger {
  NoOpTrigger(RangeListNode& c, float r) : RangeTrigger(c, r) {}
  void OnEnter(RangeListNode&) override {}
  void OnLeave(RangeListNode&) override {}
};

TEST_F(RangeTriggerLifetimeTest, TriggerDestructorUnlinksBounds) {
  auto& range_list = space_.GetRangeList();
  auto* center = MakeEntity(1, {0, 0, 0});

  // Drop a trigger without calling Remove.
  {
    NoOpTrigger trig(center->RangeNode(), 5.f);
    trig.Insert(range_list);
    // Trig goes out of scope here — ~RangeTrigger must detach its
    // two bound nodes. Without the safety net, the list stays in a
    // corrupt state (bound nodes pointing at a dead trigger).
  }

  // After the trigger is destroyed, the list must contain only the
  // central entity between head and tail.
  auto* n = range_list.Head().next_x_;
  EXPECT_EQ(n, &center->RangeNode());
  EXPECT_EQ(n->next_x_, &range_list.Tail());
}

}  // namespace
}  // namespace atlas
