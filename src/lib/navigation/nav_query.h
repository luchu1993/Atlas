#ifndef ATLAS_LIB_NAVIGATION_NAV_QUERY_H_
#define ATLAS_LIB_NAVIGATION_NAV_QUERY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "math/vector3.h"

namespace atlas::nav {

// Detour area ids: 0 stays non-walkable to match Recast's RC_NULL_AREA, so a
// carved span and an unset area read the same at the backend boundary.
enum class NavArea : uint8_t {
  kNull = 0,
  kGround = 1,
  kWater = 2,
  kJump = 3,
  kDoor = 4,
};

inline constexpr std::size_t kNavMaxAreas = 64;

// Per-area traversal cost plus Detour-style poly-flag include/exclude. A
// non-positive cost marks an area impassable for this query.
struct NavQueryFilter {
  NavQueryFilter() { area_cost.fill(1.0f); }
  std::array<float, kNavMaxAreas> area_cost;
  uint16_t include_flags{0xFFFF};
  uint16_t exclude_flags{0};
};

enum class NavPathStatus : uint8_t {
  kReached,  // full path to the goal
  kPartial,  // path to the closest reachable point (unreachable goal / node budget)
  kEmpty,    // no path and no progress (off-mesh endpoints, or no navmesh)
};

struct NavPoint {
  bool on_mesh{false};
  math::Vector3 position{0.0f, 0.0f, 0.0f};
};

struct NavPath {
  NavPathStatus status{NavPathStatus::kEmpty};
  std::vector<math::Vector3> waypoints;
  float length_m{0.0f};
};

struct NavRaycastHit {
  bool blocked{false};
  float t{1.0f};  // fraction of start->end before the first wall
  math::Vector3 position{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
};

// One instance is NOT safe to query concurrently: backends hold a mutable
// search node pool. Pool a query per worker thread for parallel pathfinds.
class NavQuery {
 public:
  virtual ~NavQuery() = default;

  [[nodiscard]] virtual auto FindPath(const math::Vector3& start, const math::Vector3& end,
                                      const NavQueryFilter& filter) const -> NavPath = 0;
  [[nodiscard]] virtual auto NearestPoint(const math::Vector3& point,
                                          const math::Vector3& half_extents,
                                          const NavQueryFilter& filter) const -> NavPoint = 0;
  [[nodiscard]] virtual auto Raycast(const math::Vector3& start, const math::Vector3& end,
                                     const NavQueryFilter& filter) const -> NavRaycastHit = 0;
};

// No navmesh: every path is empty, every point is off-mesh. Lets movement / AI
// tests run without the Recast backend; never fabricates a straight-line path.
class NullNavQuery final : public NavQuery {
 public:
  [[nodiscard]] auto FindPath(const math::Vector3& start, const math::Vector3& end,
                              const NavQueryFilter& filter) const -> NavPath override;
  [[nodiscard]] auto NearestPoint(const math::Vector3& point, const math::Vector3& half_extents,
                                  const NavQueryFilter& filter) const -> NavPoint override;
  [[nodiscard]] auto Raycast(const math::Vector3& start, const math::Vector3& end,
                             const NavQueryFilter& filter) const -> NavRaycastHit override;
};

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_NAV_QUERY_H_
