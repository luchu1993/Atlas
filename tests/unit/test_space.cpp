#include <filesystem>
#include <memory>

#include <gtest/gtest.h>

#include "cell.h"
#include "cell_entity.h"
#include "cellappmgr/bsp_tree.h"
#include "math/vector3.h"
#include "physics/collision_asset.h"
#include "physics/physics_query.h"
#include "platform/filesystem.h"
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
      100, /*type_id=*/uint16_t{1}, space, math::Vector3{5, 0, 5}, math::Vector3{1, 0, 0}));
  EXPECT_EQ(space.EntityCount(), 1u);
  EXPECT_EQ(space.FindEntity(100), entity);
  // This minimal case has only head, entity, tail, so a direct check works.
  EXPECT_EQ(space.GetRangeList().Head().next_x_, &entity->RangeNode());
}

TEST(Space, RemoveEntityUnlinksFromRangeList) {
  Space space(1);
  space.AddEntity(std::make_unique<CellEntity>(100, uint16_t{1}, space, math::Vector3{0, 0, 0},
                                               math::Vector3{1, 0, 0}));
  space.AddEntity(std::make_unique<CellEntity>(200, uint16_t{1}, space, math::Vector3{5, 0, 5},
                                               math::Vector3{1, 0, 0}));
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
  auto* a = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{10, 0, 0}, math::Vector3{1, 0, 0}));
  auto* b = space.AddEntity(std::make_unique<CellEntity>(
      2, uint16_t{1}, space, math::Vector3{20, 0, 0}, math::Vector3{1, 0, 0}));

  // Initially X order: head, a (x=10), b (x=20), tail.
  ASSERT_EQ(space.GetRangeList().Head().next_x_, &a->RangeNode());

  // Move a past b on X.
  a->SetPosition(math::Vector3{30, 0, 0});
  // Now X order: head, b (x=20), a (x=30), tail.
  EXPECT_EQ(space.GetRangeList().Head().next_x_, &b->RangeNode());
}

TEST(Space, TickDrivesControllersOnEveryEntity) {
  Space space(1);
  auto* entity = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));

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
  // Destroy only marks; RemoveEntity is the owning erase path.
  Space space(1);
  auto* entity = space.AddEntity(std::make_unique<CellEntity>(
      1, uint16_t{1}, space, math::Vector3{0, 0, 0}, math::Vector3{1, 0, 0}));
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

TEST(Space, PhysicsQueryCanBeReplaced) {
  Space space(1);
  EXPECT_NE(dynamic_cast<physics::StaticPhysicsQuery*>(&space.PhysicsQuery()), nullptr);

  space.SetPhysicsQuery(std::make_unique<physics::FlatPhysicsQuery>(-2.0f));

  physics::GroundProbeQuery query;
  query.origin = {0.0f, 0.0f, 0.0f};
  query.max_distance_m = 3.0f;
  const auto hit = space.PhysicsQuery().GroundProbe(query);
  ASSERT_TRUE(hit.hit);
  EXPECT_FLOAT_EQ(hit.position.y, -2.0f);
}

TEST(Space, CollisionAssetInstallsStaticPhysicsQuery) {
  auto asset = physics::LoadCollisionAssetFromJson(R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "space-unit",
    "objects": [
      {"shape": "box", "min": [-1, 2, -1], "max": [1, 3, 1], "layer": 4}
    ]
  })");
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();

  Space space(1);
  space.SetCollisionAsset(*asset);

  EXPECT_EQ(space.CollisionAssetSourceHash(), "space-unit");
  EXPECT_EQ(space.CollisionAssetObjectCount(), 1u);

  physics::GroundProbeQuery query;
  query.origin = {0.0f, 5.0f, 0.0f};
  query.max_distance_m = 4.0f;
  query.radius_m = 0.1f;
  query.filter.mask.bits = 1u << 4;

  const auto hit = space.PhysicsQuery().GroundProbe(query);
  ASSERT_TRUE(hit.hit);
  EXPECT_FLOAT_EQ(hit.position.y, 3.0f);
  EXPECT_EQ(hit.layer, 4u);

  query.filter.mask.bits = 1u;
  EXPECT_FALSE(space.PhysicsQuery().GroundProbe(query).hit);
}

TEST(Space, LoadCollisionAssetFromFileKeepsPreviousQueryOnFailure) {
  auto asset = physics::LoadCollisionAssetFromJson(R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "kept",
    "objects": [
      {"shape": "plane", "point": [0, 2, 0], "normal": [0, 1, 0], "layer": 3}
    ]
  })");
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();

  Space space(1);
  space.SetCollisionAsset(*asset);

  const auto path = fs::TempDirectory() / "atlas_space_bad_collision.json";
  ASSERT_TRUE(fs::WriteTextFile(path, "not json").HasValue());
  auto result = space.LoadCollisionAssetFromFile(path);

  ASSERT_FALSE(result.HasValue());
  EXPECT_EQ(space.CollisionAssetSourceHash(), "kept");
  EXPECT_EQ(space.CollisionAssetObjectCount(), 1u);

  physics::GroundProbeQuery query;
  query.origin = {0.0f, 5.0f, 0.0f};
  query.max_distance_m = 4.0f;
  query.filter.mask.bits = 1u << 3;
  EXPECT_TRUE(space.PhysicsQuery().GroundProbe(query).hit);
  std::filesystem::remove(path);
}

TEST(Space, SetPhysicsQueryClearsCollisionAssetMetadata) {
  auto asset = physics::LoadCollisionAssetFromJson(R"({
    "version": 1,
    "coordinate_system": "x_right_y_up_z_forward_meters",
    "source_hash": "asset",
    "objects": []
  })");
  ASSERT_TRUE(asset.HasValue()) << asset.Error().Message();

  Space space(1);
  space.SetCollisionAsset(*asset);
  space.SetPhysicsQuery(std::make_unique<physics::FlatPhysicsQuery>(-1.0f));

  EXPECT_TRUE(space.CollisionAssetSourceHash().empty());
  EXPECT_EQ(space.CollisionAssetObjectCount(), 0u);
}

TEST(Space, ForEachEntityIteratesAll) {
  Space space(1);
  space.AddEntity(std::make_unique<CellEntity>(1, uint16_t{1}, space, math::Vector3{0, 0, 0},
                                               math::Vector3{1, 0, 0}));
  space.AddEntity(std::make_unique<CellEntity>(2, uint16_t{1}, space, math::Vector3{0, 0, 0},
                                               math::Vector3{1, 0, 0}));
  space.AddEntity(std::make_unique<CellEntity>(3, uint16_t{1}, space, math::Vector3{0, 0, 0},
                                               math::Vector3{1, 0, 0}));

  int count = 0;
  space.ForEachEntity([&count](CellEntity&) { ++count; });
  EXPECT_EQ(count, 3);
}

namespace {
auto Bytes(std::initializer_list<uint8_t> bs) -> std::vector<uint8_t> { return {bs}; }
}  // namespace

TEST(SpaceData, EmptyByDefault) {
  Space space(1);
  EXPECT_TRUE(space.Data().Empty());
  EXPECT_EQ(space.Data().Size(), 0u);
  EXPECT_EQ(space.Data().Get(42), nullptr);
  EXPECT_FALSE(space.Data().Contains(42));
}

TEST(SpaceData, SetInsertsAndReturnsTrue) {
  Space space(1);
  auto v = Bytes({1, 2, 3});
  EXPECT_TRUE(space.Data().Set(10, v));
  ASSERT_NE(space.Data().Get(10), nullptr);
  EXPECT_EQ(*space.Data().Get(10), v);
  EXPECT_TRUE(space.Data().Contains(10));
  EXPECT_EQ(space.Data().Size(), 1u);
}

TEST(SpaceData, SetSameValueReturnsFalse) {
  Space space(1);
  auto v = Bytes({7, 7, 7});
  EXPECT_TRUE(space.Data().Set(5, v));
  EXPECT_FALSE(space.Data().Set(5, v));
  EXPECT_EQ(space.Data().Size(), 1u);
}

TEST(SpaceData, SetDifferentValueOverwrites) {
  Space space(1);
  EXPECT_TRUE(space.Data().Set(1, Bytes({1})));
  EXPECT_TRUE(space.Data().Set(1, Bytes({2, 3})));
  EXPECT_EQ(*space.Data().Get(1), Bytes({2, 3}));
}

TEST(SpaceData, RemoveErasesExistingKey) {
  Space space(1);
  space.Data().Set(1, Bytes({1}));
  space.Data().Set(2, Bytes({2}));
  EXPECT_TRUE(space.Data().Remove(1));
  EXPECT_FALSE(space.Data().Contains(1));
  EXPECT_TRUE(space.Data().Contains(2));
  EXPECT_EQ(space.Data().Size(), 1u);
}

TEST(SpaceData, RemoveMissingReturnsFalse) {
  Space space(1);
  EXPECT_FALSE(space.Data().Remove(999));
}

TEST(SpaceData, SnapshotIsOrderedByKeyId) {
  Space space(1);
  space.Data().Set(30, Bytes({0xC}));
  space.Data().Set(10, Bytes({0xA}));
  space.Data().Set(20, Bytes({0xB}));

  auto snap = space.Data().Snapshot();
  ASSERT_EQ(snap.size(), 3u);
  EXPECT_EQ(snap[0].first, 10u);
  EXPECT_EQ(snap[1].first, 20u);
  EXPECT_EQ(snap[2].first, 30u);
  EXPECT_EQ(snap[0].second, Bytes({0xA}));
}

TEST(SpaceData, ClearWipesAll) {
  Space space(1);
  space.Data().Set(1, Bytes({1}));
  space.Data().Set(2, Bytes({2}));
  space.Data().Clear();
  EXPECT_TRUE(space.Data().Empty());
}

namespace {

auto MakeLeafInfo(cellappmgr::CellID id, uint16_t port) -> CellInfo {
  CellInfo info;
  info.cell_id = id;
  info.cellapp_addr = Address(0x7F000001u, port);
  return info;
}

auto MakeSingleCellTree(cellappmgr::CellID id, uint16_t port) -> BSPTree {
  BSPTree t;
  t.InitSingleCell(MakeLeafInfo(id, port));
  return t;
}

}  // namespace

TEST(SpaceOwner, NoBspTreeReturnsFalse) {
  Space space(1);
  EXPECT_FALSE(space.IsOwner());
}

TEST(SpaceOwner, BspTreeButNoLocalCellReturnsFalse) {
  Space space(1);
  space.SetBspTree(MakeSingleCellTree(7, 30001));
  EXPECT_FALSE(space.IsOwner());
}

TEST(SpaceOwner, HoldsPrimaryCellReturnsTrue) {
  Space space(1);
  space.SetBspTree(MakeSingleCellTree(7, 30001));
  space.AddLocalCell(std::make_unique<Cell>(space, 7, CellBounds{}));
  EXPECT_TRUE(space.IsOwner());
}

TEST(SpaceOwner, HoldsOnlyNonPrimaryCellReturnsFalse) {
  Space space(1);
  BSPTree t = MakeSingleCellTree(7, 30001);
  ASSERT_TRUE(t.Split(7, BSPAxis::kX, 0.f, MakeLeafInfo(99, 30002)).HasValue());
  space.SetBspTree(std::move(t));
  space.AddLocalCell(std::make_unique<Cell>(space, 99, CellBounds{}));
  EXPECT_FALSE(space.IsOwner());
}

TEST(SpaceOwner, HoldsPrimaryAndNonPrimaryReturnsTrue) {
  Space space(1);
  BSPTree t = MakeSingleCellTree(7, 30001);
  ASSERT_TRUE(t.Split(7, BSPAxis::kX, 0.f, MakeLeafInfo(99, 30002)).HasValue());
  space.SetBspTree(std::move(t));
  space.AddLocalCell(std::make_unique<Cell>(space, 7, CellBounds{}));
  space.AddLocalCell(std::make_unique<Cell>(space, 99, CellBounds{}));
  EXPECT_TRUE(space.IsOwner());
}

TEST(SpaceData, ForEachVisitsAllInOrder) {
  Space space(1);
  space.Data().Set(2, Bytes({0xB}));
  space.Data().Set(1, Bytes({0xA}));
  space.Data().Set(3, Bytes({0xC}));

  std::vector<uint16_t> seen;
  space.Data().ForEach(
      [&seen](SpaceData::KeyId k, const SpaceData::ValueBytes&) { seen.push_back(k); });
  EXPECT_EQ(seen, (std::vector<uint16_t>{1, 2, 3}));
}

}  // namespace
}  // namespace atlas
