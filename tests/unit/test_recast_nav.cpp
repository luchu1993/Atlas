#include <gtest/gtest.h>

#include "navigation/nav_input.h"
#include "navigation/nav_params.h"
#include "navigation_recast/recast_bake.h"
#include "navigation_recast/recast_nav_backend.h"
#include "physics/collision_asset.h"

namespace atlas::nav {
namespace {

[[nodiscard]] auto FloorParams() -> NavParams {
  NavParams params;
  params.source_hash = "unit";
  return params;
}

// A 20x20 box whose top face at y=0 is the only walkable surface.
[[nodiscard]] auto FloorAsset() -> physics::CollisionAsset {
  physics::CollisionAsset asset;
  asset.boxes.push_back(physics::StaticBox{{-10, -1, -10}, {10, 0, 10}, 0});
  return asset;
}

TEST(RecastNav, BakesFloorAndFindsPath) {
  const auto params = FloorParams();
  const auto input = DeriveNavInput(FloorAsset(), params);
  ASSERT_GT(input.stats.triangles, 0u);

  const RecastNavBackendFactory backend;
  auto query = backend.Bake(input.geometry, params.bake);
  ASSERT_TRUE(query.HasValue()) << query.Error().Message();

  const NavQueryFilter filter;
  const auto path = (*query)->FindPath({-8, 0, -8}, {8, 0, 8}, filter);
  EXPECT_EQ(path.status, NavPathStatus::kReached);
  EXPECT_GE(path.waypoints.size(), 2u);
  EXPECT_GT(path.length_m, 20.0f);   // straight-line corner-to-corner ~22.6 m
  EXPECT_LT(path.length_m, 40.0f);
}

TEST(RecastNav, NearestPointSnapsToFloor) {
  const auto params = FloorParams();
  const auto input = DeriveNavInput(FloorAsset(), params);
  const RecastNavBackendFactory backend;
  auto query = backend.Bake(input.geometry, params.bake);
  ASSERT_TRUE(query.HasValue()) << query.Error().Message();

  const NavQueryFilter filter;
  const auto point = (*query)->NearestPoint({0, 0.5f, 0}, {2, 2, 2}, filter);
  EXPECT_TRUE(point.on_mesh);
  EXPECT_NEAR(point.position.y, 0.0f, 0.5f);
}

TEST(RecastNav, DebugMeshReportsWalkableArea) {
  const auto params = FloorParams();
  const auto input = DeriveNavInput(FloorAsset(), params);
  auto mesh = BuildNavDebugMesh(input.geometry, params.bake);
  ASSERT_TRUE(mesh.HasValue()) << mesh.Error().Message();
  EXPECT_GT(mesh->vertices.size(), 0u);
  EXPECT_GT(mesh->indices.size(), 0u);
  EXPECT_GT(mesh->report.walkable_area_m2, 100.0f);  // 20x20 floor minus radius erosion
}

TEST(RecastNav, EmptyInputFailsToBake) {
  const auto params = FloorParams();
  physics::CollisionAsset asset;
  asset.spheres.push_back(physics::StaticSphere{{0, 0, 0}, 1.0f, 0});  // skipped → no triangles
  const auto input = DeriveNavInput(asset, params);
  ASSERT_EQ(input.stats.triangles, 0u);

  const RecastNavBackendFactory backend;
  const auto query = backend.Bake(input.geometry, params.bake);
  EXPECT_FALSE(query.HasValue());
}

}  // namespace
}  // namespace atlas::nav
