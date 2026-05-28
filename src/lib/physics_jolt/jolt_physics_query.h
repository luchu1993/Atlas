#ifndef ATLAS_LIB_PHYSICS_JOLT_JOLT_PHYSICS_QUERY_H_
#define ATLAS_LIB_PHYSICS_JOLT_JOLT_PHYSICS_QUERY_H_

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "foundation/error.h"
#include "math/vector3.h"
#include "physics/physics_query.h"

namespace atlas::physics {

class JoltPhysicsQuery final : public PhysicsQuery {
 public:
  JoltPhysicsQuery();
  ~JoltPhysicsQuery() override;

  JoltPhysicsQuery(const JoltPhysicsQuery&) = delete;
  auto operator=(const JoltPhysicsQuery&) -> JoltPhysicsQuery& = delete;
  JoltPhysicsQuery(JoltPhysicsQuery&&) = delete;
  auto operator=(JoltPhysicsQuery&&) -> JoltPhysicsQuery& = delete;

  void AddBox(const StaticBox& box);
  void AddMesh(std::span<const math::Vector3> vertices,
               std::span<const uint32_t> indices, ObjectLayer layer);

  // Serialize a Jolt mesh shape (BVH + verts/indices) to bytes that
  // AddCookedMeshShape can later restore without rebuilding the BVH.
  [[nodiscard]] static auto CookMeshShape(std::span<const math::Vector3> vertices,
                                          std::span<const uint32_t> indices)
      -> Result<std::vector<std::byte>>;
  [[nodiscard]] auto AddCookedMeshShape(std::span<const std::byte> cooked,
                                        ObjectLayer layer) -> Result<void>;

  // Encodes Jolt version + feature bits; any cooked blob produced under a
  // different stamp must be re-cooked before it is safe to restore.
  [[nodiscard]] static auto CurrentJoltStamp() -> uint64_t;

  void Clear();

  [[nodiscard]] auto GroundProbe(const GroundProbeQuery& query) const
      -> GroundHit override;
  [[nodiscard]] auto Raycast(const RaycastQuery& query) const
      -> RaycastHit override;
  [[nodiscard]] auto CastCapsule(const CapsuleCastQuery& query) const
      -> ShapeCastHit override;
  [[nodiscard]] auto OverlapCapsule(const OverlapQuery& query) const
      -> bool override;
  [[nodiscard]] auto DepenetrateCapsule(const OverlapQuery& query) const
      -> DepenetrationHit override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace atlas::physics

#endif  // ATLAS_LIB_PHYSICS_JOLT_JOLT_PHYSICS_QUERY_H_
