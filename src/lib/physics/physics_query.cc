#include "physics/physics_query.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace atlas::physics {
namespace {

constexpr float kEpsilon = 1e-5f;

struct ExpandedBox {
  math::Vector3 min{0.0f, 0.0f, 0.0f};
  math::Vector3 max{0.0f, 0.0f, 0.0f};
  ObjectLayer layer{0};
};

struct DepenetrationCandidate {
  bool hit{false};
  math::Vector3 offset{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  float depth_m{0.0f};
};

struct SweepCandidate {
  bool hit{false};
  float fraction{1.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
};

struct RaycastCandidate {
  bool hit{false};
  float distance_m{0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
};

[[nodiscard]] auto IsFinite(const math::Vector3& value) -> bool {
  return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] auto IsFinite(const StaticBox& box) -> bool {
  return IsFinite(box.min) && IsFinite(box.max);
}

[[nodiscard]] auto IsFinite(const StaticPlane& plane) -> bool {
  return IsFinite(plane.point) && IsFinite(plane.normal);
}

[[nodiscard]] auto IsFinite(const Capsule& capsule) -> bool {
  return IsFinite(capsule.center) && std::isfinite(capsule.radius_m) &&
         std::isfinite(capsule.half_height_m);
}

[[nodiscard]] auto NonNegativeFinite(float value) -> float {
  return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

[[nodiscard]] auto NormalizeBox(const StaticBox& box) -> StaticBox {
  StaticBox out;
  out.min = {std::min(box.min.x, box.max.x), std::min(box.min.y, box.max.y),
             std::min(box.min.z, box.max.z)};
  out.max = {std::max(box.min.x, box.max.x), std::max(box.min.y, box.max.y),
             std::max(box.min.z, box.max.z)};
  out.layer = box.layer;
  return out;
}

[[nodiscard]] auto NormalizePlane(const StaticPlane& plane) -> StaticPlane {
  StaticPlane out = plane;
  if (!IsFinite(plane.normal)) return out;

  const float length = plane.normal.Length();
  if (length <= kEpsilon) return out;

  out.normal = plane.normal * (1.0f / length);
  if (out.normal.y < 0.0f) out.normal = out.normal * -1.0f;
  return out;
}

[[nodiscard]] auto IsValidBox(const StaticBox& box) -> bool {
  return IsFinite(box) && box.max.x > box.min.x && box.max.y > box.min.y &&
         box.max.z > box.min.z;
}

[[nodiscard]] auto IsValidPlane(const StaticPlane& plane) -> bool {
  return IsFinite(plane) && plane.normal.LengthSquared() > kEpsilon * kEpsilon &&
         plane.normal.y > kEpsilon;
}

[[nodiscard]] auto PlaneGroundY(const StaticPlane& plane,
                                const math::Vector3& origin) -> float {
  const float x_offset = origin.x - plane.point.x;
  const float z_offset = origin.z - plane.point.z;
  return plane.point.y -
         (plane.normal.x * x_offset + plane.normal.z * z_offset) / plane.normal.y;
}

[[nodiscard]] auto NormalizedDirection(const math::Vector3& direction) -> math::Vector3 {
  if (!IsFinite(direction)) return {};
  const float length = direction.Length();
  if (length <= kEpsilon) return {};
  return direction * (1.0f / length);
}

[[nodiscard]] auto CapsuleHeight(const Capsule& capsule) -> float {
  return NonNegativeFinite(capsule.half_height_m) * 2.0f;
}

[[nodiscard]] auto ExpandedBoxForCapsule(const StaticBox& box,
                                         const Capsule& capsule) -> ExpandedBox {
  const float radius = NonNegativeFinite(capsule.radius_m);
  const float height = CapsuleHeight(capsule);
  ExpandedBox out;
  out.min = {box.min.x - radius, box.min.y - height, box.min.z - radius};
  out.max = {box.max.x + radius, box.max.y, box.max.z + radius};
  out.layer = box.layer;
  return out;
}

[[nodiscard]] auto PointInside(const math::Vector3& point, const math::Vector3& min,
                               const math::Vector3& max) -> bool {
  return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y &&
         point.z >= min.z && point.z <= max.z;
}

[[nodiscard]] auto ExitExpandedBox(const math::Vector3& point,
                                   const ExpandedBox& box) -> DepenetrationCandidate {
  if (!PointInside(point, box.min, box.max)) return {};

  DepenetrationCandidate best;
  best.hit = true;
  best.depth_m = std::numeric_limits<float>::max();
  auto consider = [&best](float depth, const math::Vector3& normal) {
    if (depth < best.depth_m) {
      best.depth_m = depth;
      best.normal = normal;
      best.offset = normal * depth;
    }
  };

  consider(point.x - box.min.x, {-1.0f, 0.0f, 0.0f});
  consider(box.max.x - point.x, {1.0f, 0.0f, 0.0f});
  consider(point.y - box.min.y, {0.0f, -1.0f, 0.0f});
  consider(box.max.y - point.y, {0.0f, 1.0f, 0.0f});
  consider(point.z - box.min.z, {0.0f, 0.0f, -1.0f});
  consider(box.max.z - point.z, {0.0f, 0.0f, 1.0f});
  return best;
}

[[nodiscard]] auto SweepPointAabb(const math::Vector3& start,
                                  const math::Vector3& displacement,
                                  const ExpandedBox& box) -> SweepCandidate {
  const auto overlap = ExitExpandedBox(start, box);
  if (overlap.hit) {
    if (overlap.depth_m > kEpsilon) {
      return SweepCandidate{true, 0.0f, overlap.normal};
    }

    auto touching_and_entering = [&](float distance, const math::Vector3& normal) -> bool {
      return std::fabs(distance) <= kEpsilon && displacement.Dot(normal) < -kEpsilon;
    };
    if (touching_and_entering(start.x - box.min.x, {-1.0f, 0.0f, 0.0f})) {
      return SweepCandidate{true, 0.0f, {-1.0f, 0.0f, 0.0f}};
    }
    if (touching_and_entering(box.max.x - start.x, {1.0f, 0.0f, 0.0f})) {
      return SweepCandidate{true, 0.0f, {1.0f, 0.0f, 0.0f}};
    }
    if (touching_and_entering(start.y - box.min.y, {0.0f, -1.0f, 0.0f})) {
      return SweepCandidate{true, 0.0f, {0.0f, -1.0f, 0.0f}};
    }
    if (touching_and_entering(box.max.y - start.y, {0.0f, 1.0f, 0.0f})) {
      return SweepCandidate{true, 0.0f, {0.0f, 1.0f, 0.0f}};
    }
    if (touching_and_entering(start.z - box.min.z, {0.0f, 0.0f, -1.0f})) {
      return SweepCandidate{true, 0.0f, {0.0f, 0.0f, -1.0f}};
    }
    if (touching_and_entering(box.max.z - start.z, {0.0f, 0.0f, 1.0f})) {
      return SweepCandidate{true, 0.0f, {0.0f, 0.0f, 1.0f}};
    }
    return {};
  }

  float enter_fraction = 0.0f;
  float exit_fraction = 1.0f;
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  for (int axis = 0; axis < 3; ++axis) {
    const auto index = static_cast<std::size_t>(axis);
    const float start_value = start[index];
    const float delta_value = displacement[index];
    const float min_value = box.min[index];
    const float max_value = box.max[index];

    if (std::fabs(delta_value) <= kEpsilon) {
      if (start_value < min_value || start_value > max_value) return {};
      continue;
    }

    float near_fraction = 0.0f;
    float far_fraction = 0.0f;
    math::Vector3 axis_normal{0.0f, 0.0f, 0.0f};
    if (delta_value > 0.0f) {
      near_fraction = (min_value - start_value) / delta_value;
      far_fraction = (max_value - start_value) / delta_value;
      axis_normal[index] = -1.0f;
    } else {
      near_fraction = (max_value - start_value) / delta_value;
      far_fraction = (min_value - start_value) / delta_value;
      axis_normal[index] = 1.0f;
    }

    if (near_fraction > enter_fraction) {
      enter_fraction = near_fraction;
      normal = axis_normal;
    }
    exit_fraction = std::min(exit_fraction, far_fraction);
    if (enter_fraction > exit_fraction) return {};
  }

  if (exit_fraction < 0.0f || enter_fraction > 1.0f) return {};
  return SweepCandidate{true, std::clamp(enter_fraction, 0.0f, 1.0f), normal};
}

[[nodiscard]] auto RaycastBox(const math::Vector3& origin,
                              const math::Vector3& direction,
                              float max_distance,
                              const StaticBox& box) -> RaycastCandidate {
  if (PointInside(origin, box.min, box.max)) {
    return RaycastCandidate{true, 0.0f, direction * -1.0f};
  }

  float enter_distance = 0.0f;
  float exit_distance = max_distance;
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  for (int axis = 0; axis < 3; ++axis) {
    const auto index = static_cast<std::size_t>(axis);
    const float origin_value = origin[index];
    const float direction_value = direction[index];
    const float min_value = box.min[index];
    const float max_value = box.max[index];

    if (std::fabs(direction_value) <= kEpsilon) {
      if (origin_value < min_value || origin_value > max_value) return {};
      continue;
    }

    float near_distance = 0.0f;
    float far_distance = 0.0f;
    math::Vector3 axis_normal{0.0f, 0.0f, 0.0f};
    if (direction_value > 0.0f) {
      near_distance = (min_value - origin_value) / direction_value;
      far_distance = (max_value - origin_value) / direction_value;
      axis_normal[index] = -1.0f;
    } else {
      near_distance = (max_value - origin_value) / direction_value;
      far_distance = (min_value - origin_value) / direction_value;
      axis_normal[index] = 1.0f;
    }

    if (near_distance > enter_distance) {
      enter_distance = near_distance;
      normal = axis_normal;
    }
    exit_distance = std::min(exit_distance, far_distance);
    if (enter_distance > exit_distance) return {};
  }

  if (exit_distance < 0.0f || enter_distance > max_distance) return {};
  return RaycastCandidate{true, std::clamp(enter_distance, 0.0f, max_distance), normal};
}

[[nodiscard]] auto RaycastPlane(const math::Vector3& origin,
                                const math::Vector3& direction,
                                float max_distance,
                                const StaticPlane& plane) -> RaycastCandidate {
  const float denominator = plane.normal.Dot(direction);
  if (std::fabs(denominator) <= kEpsilon) return {};

  const float distance = plane.normal.Dot(plane.point - origin) / denominator;
  if (distance < 0.0f || distance > max_distance) return {};
  return RaycastCandidate{true, distance, plane.normal};
}

[[nodiscard]] auto PlaneSignedDistance(const math::Vector3& point,
                                       const StaticPlane& plane) -> float {
  return plane.normal.Dot(point - plane.point);
}

[[nodiscard]] auto DepenetrateBaseFromPlane(const math::Vector3& base,
                                            const StaticPlane& plane)
    -> DepenetrationCandidate {
  const float signed_distance = PlaneSignedDistance(base, plane);
  if (signed_distance >= -kEpsilon) return {};

  const float depth = -signed_distance;
  return DepenetrationCandidate{true, plane.normal * depth, plane.normal, depth};
}

[[nodiscard]] auto SweepBaseToPlane(const math::Vector3& start,
                                    const math::Vector3& displacement,
                                    const StaticPlane& plane) -> SweepCandidate {
  const float start_distance = plane.normal.Dot(start - plane.point);
  if (start_distance <= kEpsilon) return {};

  const float denominator = plane.normal.Dot(displacement);
  if (denominator >= -kEpsilon) return {};

  const float fraction = -start_distance / denominator;
  if (fraction < 0.0f || fraction > 1.0f) return {};
  return SweepCandidate{true, std::clamp(fraction, 0.0f, 1.0f), plane.normal};
}

[[nodiscard]] auto CapsuleOverlapsBox(const Capsule& capsule, const StaticBox& box) -> bool {
  const float radius = NonNegativeFinite(capsule.radius_m);
  const float base_y = capsule.center.y;
  const float top_y = base_y + CapsuleHeight(capsule);
  if (top_y <= box.min.y || base_y >= box.max.y) return false;

  const float closest_x = std::clamp(capsule.center.x, box.min.x, box.max.x);
  const float closest_z = std::clamp(capsule.center.z, box.min.z, box.max.z);
  const float dx = capsule.center.x - closest_x;
  const float dz = capsule.center.z - closest_z;
  if (radius <= kEpsilon) {
    return capsule.center.x > box.min.x && capsule.center.x < box.max.x &&
           capsule.center.z > box.min.z && capsule.center.z < box.max.z;
  }
  return dx * dx + dz * dz < radius * radius;
}

}  // namespace

auto LayerMask::Contains(ObjectLayer layer) const -> bool {
  if (layer >= 32) return false;
  return (bits & (uint32_t{1} << layer)) != 0;
}

auto PhysicsQuery::DepenetrateCapsule(const OverlapQuery&) const -> DepenetrationHit {
  return {};
}

auto PhysicsQuery::Raycast(const RaycastQuery&) const -> RaycastHit {
  return {};
}

auto NullPhysicsQuery::GroundProbe(const GroundProbeQuery&) const -> GroundHit {
  return {};
}

auto NullPhysicsQuery::CastCapsule(const CapsuleCastQuery&) const -> ShapeCastHit {
  return {};
}

auto NullPhysicsQuery::OverlapCapsule(const OverlapQuery&) const -> bool {
  return false;
}

auto FlatPhysicsQuery::GroundProbe(const GroundProbeQuery& query) const -> GroundHit {
  GroundHit hit;
  if (!query.filter.mask.Contains(0) || !IsFinite(query.origin) ||
      !std::isfinite(query.max_distance_m) || !std::isfinite(query.radius_m)) {
    return hit;
  }

  const float max_distance = NonNegativeFinite(query.max_distance_m);
  const float distance = query.origin.y - ground_y_;
  if (distance < 0.0f || distance > max_distance) return hit;

  hit.hit = true;
  hit.position = {query.origin.x, ground_y_, query.origin.z};
  hit.distance_m = distance;
  return hit;
}

auto FlatPhysicsQuery::Raycast(const RaycastQuery& query) const -> RaycastHit {
  RaycastHit hit;
  if (!query.filter.mask.Contains(0) || !IsFinite(query.origin) ||
      !std::isfinite(query.max_distance_m)) {
    return hit;
  }

  const auto direction = NormalizedDirection(query.direction);
  const float max_distance = NonNegativeFinite(query.max_distance_m);
  if (direction.LengthSquared() <= kEpsilon * kEpsilon ||
      std::fabs(direction.y) <= kEpsilon) {
    return hit;
  }

  const float distance = (ground_y_ - query.origin.y) / direction.y;
  if (distance < 0.0f || distance > max_distance) return hit;

  hit.hit = true;
  hit.fraction = max_distance > 0.0f ? distance / max_distance : 1.0f;
  hit.position = query.origin + direction * distance;
  hit.normal = {0.0f, 1.0f, 0.0f};
  hit.distance_m = distance;
  hit.layer = 0;
  return hit;
}

auto FlatPhysicsQuery::CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit {
  ShapeCastHit hit;
  if (!query.filter.mask.Contains(0) || !IsFinite(query.capsule) ||
      !IsFinite(query.displacement) || query.displacement.y >= -kEpsilon) {
    return hit;
  }

  const float start_y = query.capsule.center.y;
  const float end_y = start_y + query.displacement.y;
  if (start_y <= ground_y_ + kEpsilon || end_y > ground_y_) return hit;

  const float fraction = (ground_y_ - start_y) / query.displacement.y;
  const float cast_length = query.displacement.Length();
  hit.hit = true;
  hit.fraction = std::clamp(fraction, 0.0f, 1.0f);
  hit.position = query.capsule.center + query.displacement * hit.fraction;
  hit.normal = {0.0f, 1.0f, 0.0f};
  hit.distance_m = cast_length * hit.fraction;
  hit.layer = 0;
  return hit;
}

auto FlatPhysicsQuery::OverlapCapsule(const OverlapQuery& query) const -> bool {
  if (!query.filter.mask.Contains(0) || !IsFinite(query.capsule)) return false;
  return query.capsule.center.y < ground_y_ - kEpsilon;
}

auto FlatPhysicsQuery::DepenetrateCapsule(const OverlapQuery& query) const
    -> DepenetrationHit {
  DepenetrationHit hit;
  if (!query.filter.mask.Contains(0) || !IsFinite(query.capsule)) return hit;

  const float depth = ground_y_ - query.capsule.center.y;
  if (depth <= kEpsilon) return hit;

  hit.hit = true;
  hit.offset = {0.0f, depth, 0.0f};
  hit.normal = {0.0f, 1.0f, 0.0f};
  hit.depth_m = depth;
  hit.layer = 0;
  return hit;
}

void StaticPhysicsQuery::AddBox(const StaticBox& box) {
  const auto normalized = NormalizeBox(box);
  if (!IsValidBox(normalized)) return;
  boxes_.push_back(normalized);
}

void StaticPhysicsQuery::AddPlane(const StaticPlane& plane) {
  const auto normalized = NormalizePlane(plane);
  if (!IsValidPlane(normalized)) return;
  planes_.push_back(normalized);
}

void StaticPhysicsQuery::Clear() {
  boxes_.clear();
  planes_.clear();
}

auto StaticPhysicsQuery::GroundProbe(const GroundProbeQuery& query) const -> GroundHit {
  GroundHit best;
  if (!IsFinite(query.origin) || !std::isfinite(query.max_distance_m) ||
      !std::isfinite(query.radius_m)) {
    return best;
  }

  const float max_distance = NonNegativeFinite(query.max_distance_m);
  const float radius = NonNegativeFinite(query.radius_m);
  if (has_flat_ground_ && query.filter.mask.Contains(0)) {
    const float distance = query.origin.y - ground_y_;
    if (distance >= 0.0f && distance <= max_distance) {
      best.hit = true;
      best.position = {query.origin.x, ground_y_, query.origin.z};
      best.distance_m = distance;
      best.layer = 0;
    }
  }

  for (const auto& box : boxes_) {
    if (!query.filter.mask.Contains(box.layer)) continue;
    const float closest_x = std::clamp(query.origin.x, box.min.x, box.max.x);
    const float closest_z = std::clamp(query.origin.z, box.min.z, box.max.z);
    if (radius <= kEpsilon) {
      if (query.origin.x < box.min.x || query.origin.x > box.max.x ||
          query.origin.z < box.min.z || query.origin.z > box.max.z) {
        continue;
      }
    } else {
      const float dx = query.origin.x - closest_x;
      const float dz = query.origin.z - closest_z;
      if (dx * dx + dz * dz > radius * radius) continue;
    }

    const float distance = query.origin.y - box.max.y;
    if (distance < 0.0f || distance > max_distance) continue;
    if (best.hit && box.max.y < best.position.y - kEpsilon) continue;

    best.hit = true;
    best.position = {query.origin.x, box.max.y, query.origin.z};
    best.normal = {0.0f, 1.0f, 0.0f};
    best.distance_m = distance;
    best.layer = box.layer;
  }

  for (const auto& plane : planes_) {
    if (!query.filter.mask.Contains(plane.layer)) continue;

    const float ground_y = PlaneGroundY(plane, query.origin);
    if (!std::isfinite(ground_y)) continue;

    const float distance = query.origin.y - ground_y;
    if (distance < 0.0f || distance > max_distance) continue;
    if (best.hit && ground_y < best.position.y - kEpsilon) continue;

    best.hit = true;
    best.position = {query.origin.x, ground_y, query.origin.z};
    best.normal = plane.normal;
    best.distance_m = distance;
    best.layer = plane.layer;
  }
  return best;
}

auto StaticPhysicsQuery::Raycast(const RaycastQuery& query) const -> RaycastHit {
  RaycastHit best;
  if (!IsFinite(query.origin) || !std::isfinite(query.max_distance_m)) return best;

  const auto direction = NormalizedDirection(query.direction);
  const float max_distance = NonNegativeFinite(query.max_distance_m);
  if (direction.LengthSquared() <= kEpsilon * kEpsilon) return best;

  auto apply_candidate = [&](const RaycastCandidate& candidate, ObjectLayer layer) {
    if (!candidate.hit) return;
    if (best.hit && candidate.distance_m > best.distance_m + kEpsilon) return;

    best.hit = true;
    best.distance_m = candidate.distance_m;
    best.fraction = max_distance > 0.0f ? candidate.distance_m / max_distance : 1.0f;
    best.position = query.origin + direction * candidate.distance_m;
    best.normal = candidate.normal;
    best.layer = layer;
  };

  if (has_flat_ground_ && query.filter.mask.Contains(0) &&
      std::fabs(direction.y) > kEpsilon) {
    const float distance = (ground_y_ - query.origin.y) / direction.y;
    if (distance >= 0.0f && distance <= max_distance) {
      apply_candidate(RaycastCandidate{true, distance, {0.0f, 1.0f, 0.0f}}, 0);
    }
  }

  for (const auto& box : boxes_) {
    if (!query.filter.mask.Contains(box.layer)) continue;
    apply_candidate(RaycastBox(query.origin, direction, max_distance, box), box.layer);
  }

  for (const auto& plane : planes_) {
    if (!query.filter.mask.Contains(plane.layer)) continue;
    apply_candidate(RaycastPlane(query.origin, direction, max_distance, plane), plane.layer);
  }
  return best;
}

auto StaticPhysicsQuery::CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit {
  ShapeCastHit best;
  if (!IsFinite(query.capsule) || !IsFinite(query.displacement) ||
      query.displacement.LengthSquared() <= kEpsilon * kEpsilon) {
    return best;
  }

  const float cast_length = query.displacement.Length();
  auto apply_candidate = [&](const SweepCandidate& candidate, ObjectLayer layer) {
    if (!candidate.hit) return;
    if (best.hit && candidate.fraction > best.fraction + kEpsilon) return;

    best.hit = true;
    best.fraction = candidate.fraction;
    best.normal = candidate.normal;
    best.position = query.capsule.center + query.displacement * candidate.fraction;
    best.distance_m = cast_length * candidate.fraction;
    best.layer = layer;
  };

  if (has_flat_ground_ && query.filter.mask.Contains(0)) {
    const StaticPlane ground{{0.0f, ground_y_, 0.0f}, {0.0f, 1.0f, 0.0f}, 0};
    apply_candidate(SweepBaseToPlane(query.capsule.center, query.displacement, ground), 0);
  }

  for (const auto& plane : planes_) {
    if (!query.filter.mask.Contains(plane.layer)) continue;
    apply_candidate(SweepBaseToPlane(query.capsule.center, query.displacement, plane),
                    plane.layer);
  }

  for (const auto& box : boxes_) {
    if (!query.filter.mask.Contains(box.layer)) continue;
    const auto expanded = ExpandedBoxForCapsule(box, query.capsule);
    const auto candidate = SweepPointAabb(query.capsule.center, query.displacement, expanded);
    apply_candidate(candidate, box.layer);
  }
  return best;
}

auto StaticPhysicsQuery::OverlapCapsule(const OverlapQuery& query) const -> bool {
  if (!IsFinite(query.capsule)) return false;
  if (has_flat_ground_ && query.filter.mask.Contains(0) &&
      query.capsule.center.y < ground_y_ - kEpsilon) {
    return true;
  }
  for (const auto& plane : planes_) {
    if (!query.filter.mask.Contains(plane.layer)) continue;
    if (PlaneSignedDistance(query.capsule.center, plane) < -kEpsilon) return true;
  }
  for (const auto& box : boxes_) {
    if (query.filter.mask.Contains(box.layer) && CapsuleOverlapsBox(query.capsule, box)) {
      return true;
    }
  }
  return false;
}

auto StaticPhysicsQuery::DepenetrateCapsule(const OverlapQuery& query) const
    -> DepenetrationHit {
  DepenetrationHit best;
  if (!IsFinite(query.capsule)) return best;

  auto apply_candidate = [&](const DepenetrationCandidate& candidate, ObjectLayer layer) {
    if (!candidate.hit || candidate.depth_m <= kEpsilon) return;
    if (best.hit && candidate.depth_m > best.depth_m + kEpsilon) return;

    best.hit = true;
    best.offset = candidate.offset;
    best.normal = candidate.normal;
    best.depth_m = candidate.depth_m;
    best.layer = layer;
  };

  if (has_flat_ground_ && query.filter.mask.Contains(0)) {
    const StaticPlane ground{{0.0f, ground_y_, 0.0f}, {0.0f, 1.0f, 0.0f}, 0};
    apply_candidate(DepenetrateBaseFromPlane(query.capsule.center, ground), 0);
  }

  for (const auto& plane : planes_) {
    if (!query.filter.mask.Contains(plane.layer)) continue;
    apply_candidate(DepenetrateBaseFromPlane(query.capsule.center, plane), plane.layer);
  }

  for (const auto& box : boxes_) {
    if (!query.filter.mask.Contains(box.layer) || !CapsuleOverlapsBox(query.capsule, box)) {
      continue;
    }

    const auto candidate = ExitExpandedBox(query.capsule.center,
                                           ExpandedBoxForCapsule(box, query.capsule));
    apply_candidate(candidate, box.layer);
  }
  return best;
}

}  // namespace atlas::physics
