#include "physics_jolt/jolt_physics_query.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <sstream>
#include <string>
#include <string_view>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Core.h>
#include <Jolt/Core/StreamWrapper.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

namespace atlas::physics {

namespace {

constexpr float kEpsilon = 1e-5f;
constexpr JPH::ObjectLayer kStaticObjectLayer = 0;

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

[[nodiscard]] auto IsFiniteVec(const math::Vector3& v) -> bool {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Atlas Capsule: center.y is the capsule bottom, half_height_m is half of the
// total height (caps included). Jolt's CapsuleShape takes the cylinder's half.
[[nodiscard]] auto JoltCapsuleHalfHeight(float atlas_half_height_m, float radius_m) -> float {
  return std::max(0.0f, atlas_half_height_m - radius_m);
}

[[nodiscard]] auto JoltCapsuleCenter(const Capsule& capsule) -> JPH::RVec3 {
  return JPH::RVec3{capsule.center.x, capsule.center.y + capsule.half_height_m,
                    capsule.center.z};
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

void JoltPhysicsQuery::AddMesh(std::span<const math::Vector3> vertices,
                                std::span<const uint32_t> indices,
                                ObjectLayer /*layer*/) {
  if (vertices.empty() || indices.size() < 3 || (indices.size() % 3) != 0) return;

  JPH::VertexList jolt_vertices;
  jolt_vertices.reserve(vertices.size());
  for (const auto& v : vertices) {
    jolt_vertices.emplace_back(v.x, v.y, v.z);
  }
  JPH::IndexedTriangleList jolt_tris;
  jolt_tris.reserve(indices.size() / 3);
  for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
    if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
        indices[i + 2] >= vertices.size()) {
      return;
    }
    jolt_tris.emplace_back(indices[i], indices[i + 1], indices[i + 2]);
  }

  JPH::MeshShapeSettings shape_settings(std::move(jolt_vertices), std::move(jolt_tris));
  shape_settings.SetEmbedded();
  auto shape_result = shape_settings.Create();
  if (shape_result.HasError()) return;

  // Single-layer scheme until the Atlas → Jolt layer table lands.
  JPH::BodyCreationSettings bcs(shape_result.Get(), JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                kStaticObjectLayer);
  impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
  impl_->needs_optimize = true;
}

void JoltPhysicsQuery::Clear() {
  impl_ = std::make_unique<Impl>();
}

auto JoltPhysicsQuery::CookMeshShape(std::span<const math::Vector3> vertices,
                                     std::span<const uint32_t> indices)
    -> Result<std::vector<std::byte>> {
  if (vertices.empty() || indices.size() < 3 || (indices.size() % 3) != 0) {
    return Error{ErrorCode::kInvalidArgument,
                 "CookMeshShape: vertices empty or index count not multiple of 3"};
  }

  JPH::VertexList jolt_vertices;
  jolt_vertices.reserve(vertices.size());
  for (const auto& v : vertices) jolt_vertices.emplace_back(v.x, v.y, v.z);

  JPH::IndexedTriangleList jolt_tris;
  jolt_tris.reserve(indices.size() / 3);
  for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
    if (indices[i] >= vertices.size() || indices[i + 1] >= vertices.size() ||
        indices[i + 2] >= vertices.size()) {
      return Error{ErrorCode::kInvalidArgument,
                   "CookMeshShape: index out of vertex range"};
    }
    jolt_tris.emplace_back(indices[i], indices[i + 1], indices[i + 2]);
  }

  JPH::MeshShapeSettings shape_settings(std::move(jolt_vertices), std::move(jolt_tris));
  shape_settings.SetEmbedded();
  auto shape_result = shape_settings.Create();
  if (shape_result.HasError()) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("CookMeshShape: Jolt MeshShape::Create failed: {}",
                             std::string_view(shape_result.GetError().c_str(),
                                              shape_result.GetError().size()))};
  }

  std::ostringstream stream(std::ios::binary);
  JPH::StreamOutWrapper writer(stream);
  shape_result.Get()->SaveBinaryState(writer);
  const auto bytes = stream.str();
  std::vector<std::byte> out(bytes.size());
  std::memcpy(out.data(), bytes.data(), bytes.size());
  return out;
}

auto JoltPhysicsQuery::AddCookedMeshShape(std::span<const std::byte> cooked,
                                          ObjectLayer /*layer*/) -> Result<void> {
  if (cooked.empty()) {
    return Error{ErrorCode::kInvalidArgument, "AddCookedMeshShape: empty blob"};
  }

  std::string buffer(reinterpret_cast<const char*>(cooked.data()), cooked.size());
  std::istringstream stream(std::move(buffer), std::ios::binary);
  JPH::StreamInWrapper reader(stream);

  auto restored = JPH::Shape::sRestoreFromBinaryState(reader);
  if (restored.HasError()) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("AddCookedMeshShape: restore failed: {}",
                             std::string_view(restored.GetError().c_str(),
                                              restored.GetError().size()))};
  }

  JPH::BodyCreationSettings bcs(restored.Get(), JPH::RVec3::sZero(),
                                JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                kStaticObjectLayer);
  impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
  impl_->needs_optimize = true;
  return {};
}

auto JoltPhysicsQuery::CurrentJoltStamp() -> uint64_t {
  using JPH::uint64;
  return static_cast<uint64_t>(JPH_VERSION_ID);
}

namespace {

void AppendU32(std::vector<std::byte>& out, uint32_t v) {
  const auto* p = reinterpret_cast<const std::byte*>(&v);
  out.insert(out.end(), p, p + sizeof(v));
}

[[nodiscard]] auto ReadU32(std::span<const std::byte> bytes, std::size_t& cursor,
                            std::string_view what) -> Result<uint32_t> {
  if (cursor + sizeof(uint32_t) > bytes.size()) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("cooked-mesh blob: truncated reading {}", what)};
  }
  uint32_t v = 0;
  std::memcpy(&v, bytes.data() + cursor, sizeof(v));
  cursor += sizeof(v);
  return v;
}

}  // namespace

auto JoltPhysicsQuery::CookCollisionMeshes(std::span<const MeshGeometry> meshes)
    -> Result<std::vector<std::byte>> {
  std::vector<std::byte> out;
  AppendU32(out, static_cast<uint32_t>(meshes.size()));
  for (const auto& mesh : meshes) {
    auto cooked = CookMeshShape(mesh.vertices, mesh.indices);
    if (!cooked) return cooked.Error();
    AppendU32(out, static_cast<uint32_t>(mesh.layer));
    AppendU32(out, static_cast<uint32_t>(cooked->size()));
    out.insert(out.end(), cooked->begin(), cooked->end());
  }
  return out;
}

auto JoltPhysicsQuery::RestoreCookedMeshes(std::span<const std::byte> blob)
    -> Result<std::size_t> {
  if (blob.empty()) return std::size_t{0};
  std::size_t cursor = 0;
  auto count = ReadU32(blob, cursor, "mesh_count");
  if (!count) return count.Error();
  for (uint32_t i = 0; i < *count; ++i) {
    auto layer = ReadU32(blob, cursor, "layer");
    if (!layer) return layer.Error();
    auto len = ReadU32(blob, cursor, "blob_len");
    if (!len) return len.Error();
    if (cursor + *len > blob.size()) {
      return Error{ErrorCode::kInvalidArgument,
                   std::format("cooked-mesh blob: truncated reading shape {}", i)};
    }
    std::span<const std::byte> shape_bytes(blob.data() + cursor, *len);
    auto added = AddCookedMeshShape(shape_bytes, static_cast<ObjectLayer>(*layer));
    if (!added) return added.Error();
    cursor += *len;
  }
  return static_cast<std::size_t>(*count);
}

auto JoltPhysicsQuery::GroundProbe(const GroundProbeQuery& query) const -> GroundHit {
  GroundHit out;
  if (!IsFiniteVec(query.origin) || !std::isfinite(query.max_distance_m) ||
      !std::isfinite(query.radius_m)) {
    return out;
  }
  const float max_distance = query.max_distance_m > 0.0f ? query.max_distance_m : 0.0f;
  if (max_distance <= kEpsilon) return out;
  const float radius = std::max(query.radius_m, kEpsilon);

  impl_->EnsureOptimized();

  JPH::SphereShape sphere(radius);
  sphere.SetEmbedded();

  // Sphere bottom starts at origin so distance is the foot-to-ground gap, not
  // sphere-center-to-ground (matches StaticPhysicsQuery convention).
  const JPH::RMat44 start = JPH::RMat44::sTranslation(
      JPH::RVec3{query.origin.x, query.origin.y + radius, query.origin.z});
  const JPH::Vec3 motion{0.0f, -max_distance, 0.0f};
  JPH::RShapeCast cast(&sphere, JPH::Vec3::sReplicate(1.0f), start, motion);

  JPH::ShapeCastSettings settings;
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
  impl_->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(),
                                                collector);
  if (!collector.HadHit()) return out;

  const auto& hit = collector.mHit;
  out.hit = true;
  out.distance_m = hit.mFraction * max_distance;
  out.position = {query.origin.x, query.origin.y - out.distance_m, query.origin.z};
  out.layer = kStaticObjectLayer;

  JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), hit.mBodyID2);
  if (lock.Succeeded()) {
    const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(
        hit.mSubShapeID2,
        JPH::RVec3{out.position.x, out.position.y, out.position.z});
    out.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
  }
  return out;
}

auto JoltPhysicsQuery::Raycast(const RaycastQuery& query) const -> RaycastHit {
  RaycastHit out;
  if (!IsFiniteVec(query.origin) || !std::isfinite(query.max_distance_m)) return out;
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
  if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) return out;

  out.hit = true;
  out.fraction = result.mFraction;
  out.distance_m = max_distance * result.mFraction;
  out.position = {query.origin.x + direction.x * out.distance_m,
                  query.origin.y + direction.y * out.distance_m,
                  query.origin.z + direction.z * out.distance_m};
  out.layer = kStaticObjectLayer;

  JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), result.mBodyID);
  if (lock.Succeeded()) {
    const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(
        result.mSubShapeID2,
        JPH::RVec3{out.position.x, out.position.y, out.position.z});
    out.normal = {normal.GetX(), normal.GetY(), normal.GetZ()};
  }
  return out;
}

auto JoltPhysicsQuery::CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit {
  ShapeCastHit out;
  if (!IsFiniteVec(query.capsule.center) || !std::isfinite(query.capsule.radius_m) ||
      !std::isfinite(query.capsule.half_height_m) || !IsFiniteVec(query.displacement)) {
    return out;
  }
  if (query.capsule.radius_m <= kEpsilon) return out;
  if (query.displacement.LengthSquared() <= kEpsilon * kEpsilon) return out;

  impl_->EnsureOptimized();

  JPH::CapsuleShape capsule(
      JoltCapsuleHalfHeight(query.capsule.half_height_m, query.capsule.radius_m),
      query.capsule.radius_m);
  capsule.SetEmbedded();

  const JPH::RMat44 start = JPH::RMat44::sTranslation(JoltCapsuleCenter(query.capsule));
  const JPH::Vec3 motion{query.displacement.x, query.displacement.y, query.displacement.z};
  JPH::RShapeCast cast(&capsule, JPH::Vec3::sReplicate(1.0f), start, motion);

  JPH::ShapeCastSettings settings;
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
  impl_->system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(),
                                                collector);
  if (!collector.HadHit()) return out;

  const auto& hit = collector.mHit;
  out.hit = true;
  out.fraction = hit.mFraction;
  out.distance_m = query.displacement.Length() * hit.mFraction;
  out.position = {query.capsule.center.x + query.displacement.x * hit.mFraction,
                  query.capsule.center.y + query.displacement.y * hit.mFraction,
                  query.capsule.center.z + query.displacement.z * hit.mFraction};
  out.layer = kStaticObjectLayer;

  // mPenetrationAxis points into the obstacle; surface normal is the opposite.
  // Defensive normalize: Jolt does not guarantee unit length at edge contacts.
  JPH::Vec3 axis = hit.mPenetrationAxis;
  const float axis_len = axis.Length();
  if (axis_len > kEpsilon) {
    axis = axis / axis_len;
    out.normal = {-axis.GetX(), -axis.GetY(), -axis.GetZ()};
  }
  return out;
}

auto JoltPhysicsQuery::OverlapCapsule(const OverlapQuery& query) const -> bool {
  if (!IsFiniteVec(query.capsule.center) || !std::isfinite(query.capsule.radius_m) ||
      !std::isfinite(query.capsule.half_height_m)) {
    return false;
  }
  if (query.capsule.radius_m <= kEpsilon) return false;

  impl_->EnsureOptimized();

  JPH::CapsuleShape capsule(
      JoltCapsuleHalfHeight(query.capsule.half_height_m, query.capsule.radius_m),
      query.capsule.radius_m);
  capsule.SetEmbedded();

  const JPH::RMat44 transform = JPH::RMat44::sTranslation(JoltCapsuleCenter(query.capsule));
  JPH::CollideShapeSettings settings;
  JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> collector;
  impl_->system.GetNarrowPhaseQuery().CollideShape(&capsule, JPH::Vec3::sReplicate(1.0f),
                                                   transform, settings, JPH::RVec3::sZero(),
                                                   collector);
  return collector.HadHit();
}

auto JoltPhysicsQuery::DepenetrateCapsule(const OverlapQuery& query) const -> DepenetrationHit {
  DepenetrationHit out;
  if (!IsFiniteVec(query.capsule.center) || !std::isfinite(query.capsule.radius_m) ||
      !std::isfinite(query.capsule.half_height_m)) {
    return out;
  }
  if (query.capsule.radius_m <= kEpsilon) return out;

  impl_->EnsureOptimized();

  JPH::CapsuleShape capsule(
      JoltCapsuleHalfHeight(query.capsule.half_height_m, query.capsule.radius_m),
      query.capsule.radius_m);
  capsule.SetEmbedded();

  const JPH::RMat44 transform = JPH::RMat44::sTranslation(JoltCapsuleCenter(query.capsule));
  JPH::CollideShapeSettings settings;
  // ClosestHit so we take the deepest single contact; multi-body resolution belongs
  // to the motor, not the query.
  JPH::ClosestHitCollisionCollector<JPH::CollideShapeCollector> collector;
  impl_->system.GetNarrowPhaseQuery().CollideShape(&capsule, JPH::Vec3::sReplicate(1.0f),
                                                   transform, settings, JPH::RVec3::sZero(),
                                                   collector);
  if (!collector.HadHit()) return out;

  const auto& hit = collector.mHit;
  if (hit.mPenetrationDepth <= kEpsilon) return out;

  JPH::Vec3 axis = hit.mPenetrationAxis;
  const float axis_len = axis.Length();
  if (axis_len <= kEpsilon) return out;
  axis = axis / axis_len;

  out.hit = true;
  // Atlas convention: normal points OUT of the obstacle (toward the moving capsule),
  // offset moves the capsule out by `depth` along that normal.
  const math::Vector3 normal{-axis.GetX(), -axis.GetY(), -axis.GetZ()};
  out.normal = normal;
  out.depth_m = hit.mPenetrationDepth;
  out.offset = {normal.x * out.depth_m, normal.y * out.depth_m, normal.z * out.depth_m};
  out.layer = kStaticObjectLayer;
  return out;
}

}  // namespace atlas::physics
