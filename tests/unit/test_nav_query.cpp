#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "navigation/nav_query.h"

namespace atlas::nav {
namespace {

TEST(NullNavQuery, FindPathIsEmpty) {
  NullNavQuery query;
  const NavQueryFilter filter;
  const auto path = query.FindPath({0, 0, 0}, {10, 0, 10}, filter);
  EXPECT_EQ(path.status, NavPathStatus::kEmpty);
  EXPECT_TRUE(path.waypoints.empty());
  EXPECT_FLOAT_EQ(path.length_m, 0.0f);
}

TEST(NullNavQuery, NearestPointIsOffMesh) {
  NullNavQuery query;
  const NavQueryFilter filter;
  const auto point = query.NearestPoint({1, 2, 3}, {1, 1, 1}, filter);
  EXPECT_FALSE(point.on_mesh);
}

TEST(NullNavQuery, RaycastNeverBlocks) {
  NullNavQuery query;
  const NavQueryFilter filter;
  const auto hit = query.Raycast({0, 0, 0}, {5, 0, 0}, filter);
  EXPECT_FALSE(hit.blocked);
}

TEST(NavQueryFilter, DefaultsCostToOne) {
  const NavQueryFilter filter;
  EXPECT_FLOAT_EQ(filter.area_cost[static_cast<std::size_t>(NavArea::kGround)], 1.0f);
  EXPECT_EQ(filter.include_flags, uint16_t{0xFFFF});
  EXPECT_EQ(filter.exclude_flags, uint16_t{0});
}

}  // namespace
}  // namespace atlas::nav
