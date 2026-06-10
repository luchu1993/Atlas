#ifndef ATLAS_LIB_NAVIGATION_NAV_INPUT_H_
#define ATLAS_LIB_NAVIGATION_NAV_INPUT_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "math/vector3.h"
#include "navigation/nav_params.h"
#include "navigation/nav_query.h"
#include "physics/collision_asset.h"

namespace atlas::nav {

// Triangle soup ready for Recast: flat verts, int triangle indices (3 per
// triangle), one area per triangle, and the AABB the bake should voxelize.
struct NavInputGeometry {
  std::vector<math::Vector3> vertices;
  std::vector<int32_t> indices;
  std::vector<NavArea> triangle_areas;
  math::Vector3 bounds_min{0.0f, 0.0f, 0.0f};
  math::Vector3 bounds_max{0.0f, 0.0f, 0.0f};

  [[nodiscard]] auto TriangleCount() const -> std::size_t { return triangle_areas.size(); }
};

struct NavDeriveStats {
  std::size_t boxes{0};
  std::size_t meshes{0};
  std::size_t heightfields{0};
  std::size_t skipped_convexes{0};
  std::size_t skipped_spheres{0};
  std::size_t skipped_capsules{0};
  std::size_t skipped_planes{0};
  std::size_t triangles{0};
  std::vector<std::string> warnings;
};

struct NavDeriveResult {
  NavInputGeometry geometry;
  NavDeriveStats stats;
};

// Tessellates a collision asset into Recast input under the params' layer
// roles and overrides; v1 skips convex / sphere / capsule / plane with a warning.
[[nodiscard]] auto DeriveNavInput(const physics::CollisionAsset& asset, const NavParams& params)
    -> NavDeriveResult;

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_NAV_INPUT_H_
