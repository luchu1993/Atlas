#ifndef ATLAS_LIB_PHYSICS_JOLT_JOLT_PHYSICS_QUERY_H_
#define ATLAS_LIB_PHYSICS_JOLT_JOLT_PHYSICS_QUERY_H_

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "foundation/error.h"
#include "math/vector3.h"
#include "physics/collision_asset.h"
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
  void AddPlane(const StaticPlane& plane);
  void AddSphere(const StaticSphere& sphere);
  void AddCapsule(const StaticCapsule& capsule);
  // Builds a convex hull from a point cloud (no triangle topology); cheap
  // enough to rebuild at load, so it is not part of the cooked blob.
  void AddConvexHull(std::span<const math::Vector3> points, ObjectLayer layer);
  // Square height-sample grid; rebuilt at load (not cooked). FLT_MAX = hole.
  void AddHeightField(const HeightFieldGeometry& heightfield);
  void AddMesh(std::span<const math::Vector3> vertices,
               std::span<const uint32_t> indices, ObjectLayer layer);

  // Serialize a Jolt mesh shape (BVH + verts/indices) to bytes that
  // AddCookedMeshShape can later restore without rebuilding the BVH.
  [[nodiscard]] static auto CookMeshShape(std::span<const math::Vector3> vertices,
                                          std::span<const uint32_t> indices)
      -> Result<std::vector<std::byte>>;
  [[nodiscard]] auto AddCookedMeshShape(std::span<const std::byte> cooked,
                                        ObjectLayer layer) -> Result<void>;

  // Cook every mesh in `meshes` into a single opaque blob (layer + Jolt
  // SaveBinaryState bytes per shape). Empty input → empty blob.
  [[nodiscard]] static auto CookCollisionMeshes(std::span<const MeshGeometry> meshes)
      -> Result<std::vector<std::byte>>;
  // Decode a blob from CookCollisionMeshes and add each shape as a static
  // body. Returns the number of shapes added.
  [[nodiscard]] auto RestoreCookedMeshes(std::span<const std::byte> blob)
      -> Result<std::size_t>;

  // Encodes Jolt version + feature bits; any cooked blob produced under a
  // different stamp must be re-cooked before it is safe to restore.
  [[nodiscard]] static auto CurrentJoltStamp() -> uint64_t;

  // Static bodies actually in the scene. Lets a builder detect shapes that
  // silently failed to construct (degenerate geometry, body cap exceeded).
  [[nodiscard]] auto BodyCount() const -> std::size_t;

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
