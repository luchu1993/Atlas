#include "navigation_recast/recast_nav_backend.h"

#include "navigation_recast/detour_nav_query.h"
#include "navigation_recast/recast_bake.h"

namespace atlas::nav {

auto RecastNavBackendFactory::Bake(const NavInputGeometry& input, const NavBakeParams& params) const
    -> Result<std::unique_ptr<NavQuery>> {
  auto baked = BuildNavMeshData(input, params);
  if (!baked) return baked.Error();

  // Create takes ownership of nav_data on both success and failure.
  auto query = DetourNavQuery::Create(baked->nav_data, baked->nav_data_size, params.max_search_nodes,
                                      params.vertical_query_extent_m);
  if (!query) return query.Error();
  return std::unique_ptr<NavQuery>(std::move(*query));
}

}  // namespace atlas::nav
