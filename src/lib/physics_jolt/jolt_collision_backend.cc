#include "physics_jolt/jolt_collision_backend.h"

#include <format>
#include <utility>

#include "physics_jolt/jolt_physics_query.h"

namespace atlas::physics {

auto JoltCollisionBackendFactory::BuildFromCache(const LoadedCollisionCache& cache) const
    -> Result<std::unique_ptr<PhysicsQuery>> {
  const bool has_meshes = !cache.asset.meshes.empty();
  if (has_meshes) {
    const uint64_t current = JoltPhysicsQuery::CurrentJoltStamp();
    if (cache.jolt_version_stamp != current) {
      return Error{ErrorCode::kNotSupported,
                   std::format("collision cache jolt_version_stamp 0x{:016x} != current "
                               "0x{:016x}; recook required",
                               cache.jolt_version_stamp, current)};
    }
    if (cache.cooked.empty()) {
      return Error{ErrorCode::kInvalidArgument,
                   "collision cache declares meshes but carries no cooked blob; recook "
                   "required"};
    }
  }

  auto query = std::make_unique<JoltPhysicsQuery>();
  for (const auto& box : cache.asset.boxes) query->AddBox(box);
  for (const auto& plane : cache.asset.planes) query->AddPlane(plane);
  for (const auto& sphere : cache.asset.spheres) query->AddSphere(sphere);
  for (const auto& capsule : cache.asset.capsules) query->AddCapsule(capsule);
  if (!cache.cooked.empty()) {
    auto restored = query->RestoreCookedMeshes(cache.cooked);
    if (!restored) return restored.Error();
  }
  return std::unique_ptr<PhysicsQuery>(std::move(query));
}

}  // namespace atlas::physics
