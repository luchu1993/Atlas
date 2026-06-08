#ifndef ATLAS_LIB_NAVIGATION_RECAST_RECAST_NAV_BACKEND_H_
#define ATLAS_LIB_NAVIGATION_RECAST_RECAST_NAV_BACKEND_H_

#include <memory>

#include "foundation/error.h"
#include "navigation/nav_backend.h"
#include "navigation/nav_input.h"
#include "navigation/nav_params.h"
#include "navigation/nav_query.h"

namespace atlas::nav {

// Recast/Detour implementation of the nav backend: bakes a single-tile navmesh
// and returns a DetourNavQuery. The only type the server links against here is
// this factory; Recast/Detour stay inside this library.
class RecastNavBackendFactory final : public NavBackendFactory {
 public:
  [[nodiscard]] auto Bake(const NavInputGeometry& input, const NavBakeParams& params) const
      -> Result<std::unique_ptr<NavQuery>> override;
};

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_RECAST_RECAST_NAV_BACKEND_H_
