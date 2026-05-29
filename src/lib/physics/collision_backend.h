#ifndef ATLAS_LIB_PHYSICS_COLLISION_BACKEND_H_
#define ATLAS_LIB_PHYSICS_COLLISION_BACKEND_H_

#include <memory>

#include "foundation/error.h"
#include "physics/collision_asset.h"
#include "physics/physics_query.h"

namespace atlas::physics {

// Turns a loaded collision cache into a runtime PhysicsQuery. The concrete
// implementation lives in the backend library (physics_jolt); server/cellapp
// inject it at startup so those layers never include backend headers.
class CollisionBackendFactory {
 public:
  virtual ~CollisionBackendFactory() = default;

  // Mesh-bearing caches must carry a matching version stamp and a non-empty
  // cooked blob, otherwise the build fails — never a silent geometry downgrade.
  [[nodiscard]] virtual auto BuildFromCache(const LoadedCollisionCache& cache) const
      -> Result<std::unique_ptr<PhysicsQuery>> = 0;
};

}  // namespace atlas::physics

#endif  // ATLAS_LIB_PHYSICS_COLLISION_BACKEND_H_
