#ifndef ATLAS_LIB_PHYSICS_JOLT_JOLT_COLLISION_BACKEND_H_
#define ATLAS_LIB_PHYSICS_JOLT_JOLT_COLLISION_BACKEND_H_

#include <memory>

#include "physics/collision_backend.h"

namespace atlas::physics {

// Builds JoltPhysicsQuery instances from cooked collision caches. Stateless;
// requires jolt::Initialize() to have run before any BuildFromCache call.
class JoltCollisionBackendFactory final : public CollisionBackendFactory {
 public:
  [[nodiscard]] auto BuildFromCache(const LoadedCollisionCache& cache) const
      -> Result<std::unique_ptr<PhysicsQuery>> override;
};

}  // namespace atlas::physics

#endif  // ATLAS_LIB_PHYSICS_JOLT_JOLT_COLLISION_BACKEND_H_
