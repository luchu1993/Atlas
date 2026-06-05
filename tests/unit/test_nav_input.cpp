#include <cstddef>

#include <gtest/gtest.h>

#include "navigation/nav_input.h"
#include "navigation/nav_params.h"
#include "physics/collision_asset.h"

namespace atlas::nav {
namespace {

[[nodiscard]] auto MakeParams() -> NavParams {
  NavParams params;
  params.source_hash = "unit";
  return params;
}

TEST(NavInput, BoxBecomesTwelveTriangles) {
  physics::CollisionAsset asset;
  asset.boxes.push_back(physics::StaticBox{{-1, 0, -1}, {1, 2, 1}, 0});
  const auto result = DeriveNavInput(asset, MakeParams());
  EXPECT_EQ(result.stats.boxes, 1u);
  EXPECT_EQ(result.stats.triangles, 12u);
  EXPECT_EQ(result.geometry.indices.size(), 36u);
  EXPECT_EQ(result.geometry.triangle_areas.size(), 12u);
  for (auto area : result.geometry.triangle_areas) EXPECT_EQ(area, NavArea::kGround);
  EXPECT_FLOAT_EQ(result.geometry.bounds_min.x, -2.0f);  // box -1 minus 1.0 margin
  EXPECT_FLOAT_EQ(result.geometry.bounds_max.y, 3.0f);   // box 2 plus 1.0 margin
}

TEST(NavInput, TopFaceNormalPointsUp) {
  physics::CollisionAsset asset;
  asset.boxes.push_back(physics::StaticBox{{-1, 0, -1}, {1, 1, 1}, 0});
  const auto result = DeriveNavInput(asset, MakeParams());
  const auto& geo = result.geometry;
  bool found_up = false;
  for (std::size_t t = 0; t < geo.triangle_areas.size(); ++t) {
    const auto& a = geo.vertices[static_cast<std::size_t>(geo.indices[t * 3 + 0])];
    const auto& b = geo.vertices[static_cast<std::size_t>(geo.indices[t * 3 + 1])];
    const auto& c = geo.vertices[static_cast<std::size_t>(geo.indices[t * 3 + 2])];
    if ((b - a).Cross(c - a).y > 0.5f) found_up = true;
  }
  EXPECT_TRUE(found_up);
}

TEST(NavInput, CarveLayerTagsNull) {
  NavParams params = MakeParams();
  params.layer_roles[5] = NavRole::kCarve;
  physics::CollisionAsset asset;
  asset.boxes.push_back(physics::StaticBox{{-1, 0, -1}, {1, 1, 1}, 5});
  const auto result = DeriveNavInput(asset, params);
  ASSERT_EQ(result.stats.triangles, 12u);
  for (auto area : result.geometry.triangle_areas) EXPECT_EQ(area, NavArea::kNull);
}

TEST(NavInput, IgnoreLayerEmitsNothing) {
  NavParams params = MakeParams();
  params.layer_roles[5] = NavRole::kIgnore;
  physics::CollisionAsset asset;
  asset.boxes.push_back(physics::StaticBox{{-1, 0, -1}, {1, 1, 1}, 5});
  const auto result = DeriveNavInput(asset, params);
  EXPECT_EQ(result.stats.boxes, 0u);
  EXPECT_EQ(result.stats.triangles, 0u);
}

TEST(NavInput, ConvexAndSphereSkippedWithWarning) {
  physics::CollisionAsset asset;
  physics::ConvexGeometry convex;
  convex.vertices = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  convex.layer = 0;
  asset.convexes.push_back(convex);
  asset.spheres.push_back(physics::StaticSphere{{0, 0, 0}, 1.0f, 0});
  const auto result = DeriveNavInput(asset, MakeParams());
  EXPECT_EQ(result.stats.skipped_convexes, 1u);
  EXPECT_EQ(result.stats.skipped_spheres, 1u);
  EXPECT_EQ(result.stats.triangles, 0u);
  EXPECT_FALSE(result.stats.warnings.empty());
}

TEST(NavInput, MeshPassesThrough) {
  physics::CollisionAsset asset;
  physics::MeshGeometry mesh;
  mesh.vertices = {{-1, 0, -1}, {1, 0, -1}, {0, 0, 1}};
  mesh.indices = {0, 1, 2};
  mesh.layer = 0;
  asset.meshes.push_back(mesh);
  const auto result = DeriveNavInput(asset, MakeParams());
  EXPECT_EQ(result.stats.meshes, 1u);
  EXPECT_EQ(result.stats.triangles, 1u);
  EXPECT_EQ(result.geometry.vertices.size(), 3u);
}

TEST(NavInput, AreaTagOverrideRetagsTriangles) {
  NavParams params = MakeParams();
  NavOverrideVolume vol;
  vol.kind = NavOverrideKind::kAreaTag;
  vol.min = {-2, -2, -2};
  vol.max = {2, 3, 2};
  vol.area = NavArea::kWater;
  params.overrides.push_back(vol);
  physics::CollisionAsset asset;
  asset.boxes.push_back(physics::StaticBox{{-1, 0, -1}, {1, 1, 1}, 0});
  const auto result = DeriveNavInput(asset, params);
  ASSERT_EQ(result.stats.triangles, 12u);
  for (auto area : result.geometry.triangle_areas) EXPECT_EQ(area, NavArea::kWater);
}

TEST(NavInput, ExplicitBoundsHonored) {
  NavParams params = MakeParams();
  params.bounds.has_explicit = true;
  params.bounds.min = {-50, -5, -50};
  params.bounds.max = {50, 20, 50};
  physics::CollisionAsset asset;
  asset.boxes.push_back(physics::StaticBox{{-1, 0, -1}, {1, 1, 1}, 0});
  const auto result = DeriveNavInput(asset, params);
  EXPECT_FLOAT_EQ(result.geometry.bounds_min.x, -50.0f);
  EXPECT_FLOAT_EQ(result.geometry.bounds_max.z, 50.0f);
}

}  // namespace
}  // namespace atlas::nav
