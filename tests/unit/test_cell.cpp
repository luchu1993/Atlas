// Cell (Space sub-region) tests.

#include <memory>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "math/vector3.h"
#include "space.h"

namespace atlas {
namespace {

auto MakeRealEntity(Space& s, EntityID id) -> CellEntity* {
  return s.AddEntity(std::make_unique<CellEntity>(id, /*type_id=*/uint16_t{1}, s,
                                                  math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
}

TEST(Cell, StoresBoundsAndId) {
  Space space(1);
  CellBounds b{-100.f, -100.f, 100.f, 100.f};
  Cell cell(space, /*cell_id=*/7, b);
  EXPECT_EQ(cell.Id(), 7u);
  EXPECT_FLOAT_EQ(cell.Bounds().min_x, -100.f);
  EXPECT_FLOAT_EQ(cell.Bounds().max_z, 100.f);
  EXPECT_EQ(cell.RealEntityCount(), 0u);
  EXPECT_TRUE(cell.ShouldOffload());
}

TEST(Cell, SetBoundsMutates) {
  Space space(1);
  Cell cell(space, 1, CellBounds{});
  cell.SetBounds(CellBounds{0.f, 0.f, 10.f, 10.f});
  EXPECT_FLOAT_EQ(cell.Bounds().max_x, 10.f);
}

TEST(Cell, AddAndRemoveRealEntity) {
  Space space(1);
  Cell cell(space, 1, CellBounds{});
  auto* e1 = MakeRealEntity(space, 100);
  auto* e2 = MakeRealEntity(space, 200);
  cell.AddRealEntity(e1);
  cell.AddRealEntity(e2);
  EXPECT_EQ(cell.RealEntityCount(), 2u);
  EXPECT_TRUE(cell.HasRealEntity(e1));
  EXPECT_TRUE(cell.HasRealEntity(e2));

  EXPECT_TRUE(cell.RemoveRealEntity(e1));
  EXPECT_EQ(cell.RealEntityCount(), 1u);
  EXPECT_FALSE(cell.HasRealEntity(e1));
  EXPECT_TRUE(cell.HasRealEntity(e2));

  EXPECT_FALSE(cell.RemoveRealEntity(e1));  // already gone
}

TEST(Cell, AddIsIdempotent) {
  Space space(1);
  Cell cell(space, 1, CellBounds{});
  auto* e = MakeRealEntity(space, 100);
  cell.AddRealEntity(e);
  cell.AddRealEntity(e);  // duplicate add silently dropped
  EXPECT_EQ(cell.RealEntityCount(), 1u);
}

TEST(Cell, AddNullptrIgnored) {
  Space space(1);
  Cell cell(space, 1, CellBounds{});
  cell.AddRealEntity(nullptr);
  EXPECT_EQ(cell.RealEntityCount(), 0u);
}

TEST(Cell, ForEachRealEntityVisitsAll) {
  Space space(1);
  Cell cell(space, 1, CellBounds{});
  cell.AddRealEntity(MakeRealEntity(space, 100));
  cell.AddRealEntity(MakeRealEntity(space, 200));
  cell.AddRealEntity(MakeRealEntity(space, 300));

  int count = 0;
  EntityID id_sum = 0;
  cell.ForEachRealEntity([&](CellEntity& e) {
    ++count;
    id_sum += e.Id();
  });
  EXPECT_EQ(count, 3);
  EXPECT_EQ(id_sum, 600u);  // 100 + 200 + 300
}

TEST(Cell, ShouldOffloadToggle) {
  Space space(1);
  Cell cell(space, 1, CellBounds{});
  EXPECT_TRUE(cell.ShouldOffload());
  cell.SetShouldOffload(false);
  EXPECT_FALSE(cell.ShouldOffload());
  cell.SetShouldOffload(true);
  EXPECT_TRUE(cell.ShouldOffload());
}

}  // namespace
}  // namespace atlas
