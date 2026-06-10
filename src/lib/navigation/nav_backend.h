#ifndef ATLAS_LIB_NAVIGATION_NAV_BACKEND_H_
#define ATLAS_LIB_NAVIGATION_NAV_BACKEND_H_

#include <memory>

#include "foundation/error.h"
#include "navigation/nav_input.h"
#include "navigation/nav_params.h"
#include "navigation/nav_query.h"

namespace atlas::nav {

// Bakes derived input into a runtime NavQuery. Implemented in the backend
// library (navigation_recast) and injected, so gameplay never sees Recast.
class NavBackendFactory {
 public:
  virtual ~NavBackendFactory() = default;

  [[nodiscard]] virtual auto Bake(const NavInputGeometry& input,
                                  const NavBakeParams& params) const
      -> Result<std::unique_ptr<NavQuery>> = 0;
};

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_NAV_BACKEND_H_
