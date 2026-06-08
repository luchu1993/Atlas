#ifndef ATLAS_LIB_NAVIGATION_RECAST_DETOUR_NAV_QUERY_H_
#define ATLAS_LIB_NAVIGATION_RECAST_DETOUR_NAV_QUERY_H_

#include <cstdint>
#include <memory>

#include "foundation/error.h"
#include "navigation/nav_query.h"

class dtNavMesh;
class dtNavMeshQuery;

namespace atlas::nav {

// NavQuery backed by a single-tile Detour navmesh. Not safe to query
// concurrently — Detour's node pool is mutable per instance.
class DetourNavQuery final : public NavQuery {
 public:
  // Takes ownership of nav_data: on success the navmesh frees it on destroy,
  // on failure Create frees it before returning the error.
  [[nodiscard]] static auto Create(unsigned char* nav_data, int nav_data_size, uint32_t max_nodes,
                                   float vertical_extent_m)
      -> Result<std::unique_ptr<DetourNavQuery>>;
  ~DetourNavQuery() override;

  DetourNavQuery(const DetourNavQuery&) = delete;
  auto operator=(const DetourNavQuery&) -> DetourNavQuery& = delete;

  [[nodiscard]] auto FindPath(const math::Vector3& start, const math::Vector3& end,
                              const NavQueryFilter& filter) const -> NavPath override;
  [[nodiscard]] auto NearestPoint(const math::Vector3& point, const math::Vector3& half_extents,
                                  const NavQueryFilter& filter) const -> NavPoint override;
  [[nodiscard]] auto Raycast(const math::Vector3& start, const math::Vector3& end,
                             const NavQueryFilter& filter) const -> NavRaycastHit override;

 private:
  DetourNavQuery() = default;

  dtNavMesh* mesh_{nullptr};
  dtNavMeshQuery* query_{nullptr};
  float ext_[3]{2.0f, 1.0f, 2.0f};  // findNearestPoly half-extents (xz snap, y from params)
};

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_RECAST_DETOUR_NAV_QUERY_H_
