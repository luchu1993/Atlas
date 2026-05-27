#include "physics_jolt/jolt_physics_query.h"

#include <algorithm>
#include <cmath>

#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>

namespace atlas::physics {

namespace {

constexpr float kEpsilon = 1e-5f;
constexpr JPH::ObjectLayer kStaticObjectLayer = 0;

// M1b ships with a single broadphase layer; multi-layer mapping follows
// when collision asset v2 lands and we have real Atlas layer enums.
class StaticBPL final : public JPH::BroadPhaseLayerInterface {
 public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer /*layer*/) const override {
    return JPH::BroadPhaseLayer(0);
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer /*layer*/) const override {
    return "Static";
  }
#endif
};

class AlwaysOvBPLF final : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer /*a*/, JPH::BroadPhaseLayer /*b*/) const override {
    return true;
  }
};

class AlwaysOLPF final : public JPH::ObjectLayerPairFilter {
 public:
  bool ShouldCollide(JPH::ObjectLayer /*a*/, JPH::ObjectLayer /*b*/) const override {
    return true;
  }
};

[[nodiscard]] auto NormalizedDirection(const math::Vector3& direction) -> math::Vector3 {
  if (!std::isfinite(direction.x) || !std::isfinite(direction.y) ||
      !std::isfinite(direction.z)) {
    return {};
  }
  const float length = direction.Length();
  if (length <= kEpsilon) return {};
  return direction * (1.0f / length);
}

}  // namespace

struct JoltPhysicsQuery::Impl {
  StaticBPL bpl;
  AlwaysOvBPLF ov_bpl_filter;
  AlwaysOLPF ol_pair_filter;
  JPH::PhysicsSystem system;
  bool needs_optimize{false};

  Impl() {
    constexpr JPH::uint kMaxBodies = 1024;
    constexpr JPH::uint kNumBodyMutexes = 0;
    constexpr JPH::uint kMaxBodyPairs = 1024;
    constexpr JPH::uint kMaxContactConstraints = 1024;
    system.Init(kMaxBodies, kNumBodyMutexes, kMaxBodyPairs, kMaxContactConstraints, bpl,
                ov_bpl_filter, ol_pair_filter);
  }

  void EnsureOptimized() {
    if (!needs_optimize) return;
    system.OptimizeBroadPhase();
    needs_optimize = false;
  }
};

JoltPhysicsQuery::JoltPhysicsQuery() : impl_(std::make_unique<Impl>()) {}
JoltPhysicsQuery::~JoltPhysicsQuery() = default;

void JoltPhysicsQuery::AddBox(const StaticBox& box) {
  const math::Vector3 lo{std::min(box.min.x, box.max.x), std::min(box.min.y, box.max.y),
                         std::min(box.min.z, box.max.z)};
  const math::Vector3 hi{std::max(box.min.x, box.max.x), std::max(box.min.y, box.max.y),
                         std::max(box.min.z, box.max.z)};
  const JPH::Vec3 half_ext{(hi.x - lo.x) * 0.5f, (hi.y - lo.y) * 0.5f,
                           (hi.z - lo.z) * 0.5f};
  if (half_ext.GetX() <= kEpsilon || half_ext.GetY() <= kEpsilon ||
      half_ext.GetZ() <= kEpsilon) {
    return;
  }
  const JPH::RVec3 center{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};

  JPH::BoxShapeSettings shape_settings(half_ext);
  shape_settings.SetEmbedded();
  auto shape_result = shape_settings.Create();
  if (shape_result.HasError()) return;

  JPH::BodyCreationSettings bcs(shape_result.Get(), center, JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static, kStaticObjectLayer);
  impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
  impl_->needs_optimize = true;
}

void JoltPhysicsQuery::Clear() {
  // Destroying + re-creating the PhysicsSystem is cheaper than walking the body
  // interface to remove each body individually for a query-only scene.
  impl_ = std::make_unique<Impl>();
}

auto JoltPhysicsQuery::GroundProbe(const GroundProbeQuery& /*query*/) const -> GroundHit {
  return {};
}

auto JoltPhysicsQuery::Raycast(const RaycastQuery& query) const -> RaycastHit {
  RaycastHit out;
  if (!std::isfinite(query.origin.x) || !std::isfinite(query.origin.y) ||
      !std::isfinite(query.origin.z) || !std::isfinite(query.max_distance_m)) {
    return out;
  }
  const auto direction = NormalizedDirection(query.direction);
  if (direction.LengthSquared() <= kEpsilon * kEpsilon) return out;
  const float max_distance = query.max_distance_m > 0.0f ? query.max_distance_m : 0.0f;
  if (max_distance <= kEpsilon) return out;

  impl_->EnsureOptimized();

  const JPH::Vec3 jolt_dir{direction.x * max_distance, direction.y * max_distance,
                           direction.z * max_distance};
  const JPH::RRayCast ray{JPH::RVec3{query.origin.x, query.origin.y, query.origin.z},
                          jolt_dir};
  JPH::RayCastResult result;
  const bool hit = impl_->system.GetNarrowPhaseQuery().CastRay(ray, result);
  if (!hit) return out;

  out.hit = true;
  out.fraction = result.mFraction;
  out.distance_m = max_distance * result.mFraction;
  out.position = {query.origin.x + direction.x * out.distance_m,
                  query.origin.y + direction.y * out.distance_m,
                  query.origin.z + direction.z * out.distance_m};
  out.layer = kStaticObjectLayer;

  JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
  if (lock.Succeeded()) {
    const JPH::Body& body = lock.GetBody();
    const JPH::Vec3 normal = body.GetWorldSpaceSurfaceNormal(
        result.mSubShapeID2,
        JPH::RVec3{out.position.x, out.position.y, out.position.z});
    out.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
  }
  return out;
}

auto JoltPhysicsQuery::CastCapsule(const CapsuleCastQuery& /*query*/) const -> ShapeCastHit {
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
