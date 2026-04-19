// Space tests — Phase 10 Step 10.4.
//
// Covers: entity add/remove, RangeList integration on spawn/despawn,
// Tick driving controllers, destroyed-entity compaction.

#include <memory>

#include <gtest/gtest.h>

#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"
#include "space/controllers.h"
#include "space/timer_controller.h"

namespace atlas {
namespace {

TEST(Space, EmptyInitially) {
  Space space(1);
  EXPECT_EQ(space.Id(), 1u);
  EXPECT_EQ(space.EntityCount(), 0u);
}

TEST(Space, AddEntityLinksIntoRangeList) {
  Space space(1);
  auto* entity = space.AddEntity(std::make_unique<CellEntity>(
      100, /*type_id=*/1, space, math::Vector3{5, 0, 5}, math::Vector3{1, 0, 0}));
  EXPECT_EQ(space.EntityCount(), 1u);
  EXPECT_EQ(space.FindEntity(100), entity);
  // The head sentinel's right-neighbour on X should eventually reach the
  // entity node (via interleaved nodes from any future triggers).
  // For this minimal case the list has only head, entity, tail, so a
  // direct check works.
  EXPECT_EQ(space.GetRangeList().Head().next_x_, &entity->RangeNode());
}

TEST(Space, RemoveEntityUnlinksFromRangeList) {
  Space space(1);
  space.AddEntity(
      std::make_unique<CellEntity>(100, 1, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  space.AddEntity(
      std::make_unique<CellEntity>(200, 1, space, math::Vector3{5, 0, 5}, math::Vector3{1, 0, 0}));
  EXPECT_EQ(space.EntityCount(), 2u);

  space.RemoveEntity(100);
  EXPECT_EQ(space.EntityCount(), 1u);
  EXPECT_EQ(space.FindEntity(100), nullptr);
  EXPECT_NE(space.FindEntity(200), nullptr);
}

TEST(Space, RemoveNonExistentIdIsNoop) {
  Space space(1);
  space.RemoveEntity(999);
  EXPECT_EQ(space.EntityCount(), 0u);
}

TEST(Space, SetPositionReshufflesRangeList) {
  Space space(1);
  auto* a = space.AddEntity(
      std::make_unique<CellEntity>(1, 1, space, math::Vector3{10, 0, 0}, math::Vector3{1, 0, 0}));
  auto* b = space.AddEntity(
      std::make_unique<CellEntity>(2, 1, space, math::Vector3{20, 0, 0}, math::Vector3{1, 0, 0}));

  // Initially X order: head, a (x=10), b (x=20), tail.
  ASSERT_EQ(space.GetRangeList().Head().next_x_, &a->RangeNode());

  // Move a past b on X.
  a->SetPosition(math::Vector3{30, 0, 0});
  // Now X order: head, b (x=20), a (x=30), tail.
  EXPECT_EQ(space.GetRangeList().Head().next_x_, &b->RangeNode());
}

TEST(Space, TickDrivesControllersOnEveryEntity) {
  Space space(1);
  auto* entity = space.AddEntity(
      std::make_unique<CellEntity>(1, 1, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  int fires = 0;
  entity->GetControllers().Add(
      std::make_unique<TimerController>(
          /*interval=*/0.1f, /*repeat=*/true, [&fires](TimerController&) { ++fires; }),
      /*motion=*/nullptr, 0);

  space.Tick(0.1f);
  EXPECT_EQ(fires, 1);
  space.Tick(0.1f);
  EXPECT_EQ(fires, 2);
}

TEST(Space, DestroyMarksButDoesNotErase) {
  // Locks the intentional invariant: Destroy() flips a flag but never
  // removes the entity from Space::entities_. The only path that
  // erases the owning unique_ptr is RemoveEntity. A Space-local
  // compaction sweep was removed (space.cc:Tick) because it would be
  // a second destruction path that silently invalidates CellApp's
  // base/cell-id indexes.
  Space space(1);
  auto* entity = space.AddEntity(
      std::make_unique<CellEntity>(1, 1, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  ASSERT_EQ(space.EntityCount(), 1u);

  entity->Destroy();
  space.Tick(0.1f);

  // Entity remains in the map, flagged destroyed. Proper disposal
  // requires Space::RemoveEntity.
  EXPECT_EQ(space.EntityCount(), 1u);
  EXPECT_TRUE(entity->IsDestroyed());

  space.RemoveEntity(1);
  EXPECT_EQ(space.EntityCount(), 0u);
}

TEST(Space, ForEachEntityIteratesAll) {
  Space space(1);
  space.AddEntity(
      std::make_unique<CellEntity>(1, 1, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  space.AddEntity(
      std::make_unique<CellEntity>(2, 1, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
  space.AddEntity(
      std::make_unique<CellEntity>(3, 1, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

  int count = 0;
  space.ForEachEntity([&count](CellEntity&) { ++count; });
  EXPECT_EQ(count, 3);
}

}  // namespace
}  // namespace atlas
