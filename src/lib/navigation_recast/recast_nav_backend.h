#ifndef ATLAS_LIB_NAVIGATION_RECAST_RECAST_NAV_BACKEND_H_
#define ATLAS_LIB_NAVIGATION_RECAST_RECAST_NAV_BACKEND_H_

#include <memory>

#include "foundation/error.h"
#include "navigation/nav_backend.h"
#include "navigation/nav_input.h"
#include "navigation/nav_params.h"
#include "navigation/nav_query.h"

namespace atlas::nav {

// Recast/Detour nav backend: bakes a single-tile navmesh into a DetourNavQuery.
// The server links only this factory; Recast/Detour types stay inside this library.
class RecastNavBackendFactory final : public NavBackendFactory {
 public:
  [[nodiscard]] auto Bake(const NavInputGeometry& input, const NavBakeParams& params) const
      -> Result<std::unique_ptr<NavQuery>> override;
};

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_RECAST_RECAST_NAV_BACKEND_H_
