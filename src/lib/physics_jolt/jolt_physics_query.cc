#include "physics_jolt/jolt_physics_query.h"

namespace atlas::physics {

// M1a: Impl is empty; M1b will hold the PhysicsSystem / BodyInterface and the
// translation layer between Atlas math types and Jolt's RVec3 / Quat.
struct JoltPhysicsQuery::Impl {};

JoltPhysicsQuery::JoltPhysicsQuery() : impl_(std::make_unique<Impl>()) {}
JoltPhysicsQuery::~JoltPhysicsQuery() = default;

auto JoltPhysicsQuery::GroundProbe(const GroundProbeQuery& /*query*/) const
    -> GroundHit {
  return {};
}

auto JoltPhysicsQuery::Raycast(const RaycastQuery& /*query*/) const
    -> RaycastHit {
  return {};
}

auto JoltPhysicsQuery::CastCapsule(const CapsuleCastQuery& /*query*/) const
    -> ShapeCastHit {
  return {};
}

auto JoltPhysicsQuery::OverlapCapsule(const OverlapQuery& /*query*/) const -> bool {
  return false;
}

auto JoltPhysicsQuery::DepenetrateCapsule(const OverlapQuery& /*query*/) const
    -> DepenetrationHit {
  return {};
}

}  // namespace atlas::physics
