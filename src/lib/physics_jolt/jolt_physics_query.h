#ifndef ATLAS_LIB_PHYSICS_JOLT_JOLT_PHYSICS_QUERY_H_
#define ATLAS_LIB_PHYSICS_JOLT_JOLT_PHYSICS_QUERY_H_

#include <memory>

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
