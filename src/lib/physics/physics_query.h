#ifndef ATLAS_LIB_PHYSICS_PHYSICS_QUERY_H_
#define ATLAS_LIB_PHYSICS_PHYSICS_QUERY_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "math/vector3.h"

namespace atlas::physics {

using ObjectLayer = uint16_t;

struct LayerMask {
  uint32_t bits{0xFFFFFFFFu};

  [[nodiscard]] auto Contains(ObjectLayer layer) const -> bool;
};

struct QueryFilter {
  LayerMask mask;
};

// center.y is the capsule FOOT (not geometric center); the Jolt adapter
// bridges to `geometric_center = foot + half_height` when forwarding queries.
struct Capsule {
  math::Vector3 center{0.0f, 0.0f, 0.0f};
  float radius_m{0.0f};
  float half_height_m{0.0f};
};

struct GroundProbeQuery {
  math::Vector3 origin{0.0f, 0.0f, 0.0f};
  float max_distance_m{0.0f};
  float radius_m{0.0f};
  QueryFilter filter;
};

struct CapsuleCastQuery {
  Capsule capsule;
  math::Vector3 displacement{0.0f, 0.0f, 0.0f};
  QueryFilter filter;
};

struct OverlapQuery {
  Capsule capsule;
  QueryFilter filter;
};

struct RaycastQuery {
  math::Vector3 origin{0.0f, 0.0f, 0.0f};
  math::Vector3 direction{0.0f, 0.0f, 1.0f};
  float max_distance_m{0.0f};
  QueryFilter filter;
};

struct PhysicsQueryRegion {
  float min_x{0.0f};
  float min_z{0.0f};
  float max_x{0.0f};
  float max_z{0.0f};
};

struct StaticBox {
  math::Vector3 min{0.0f, 0.0f, 0.0f};
  math::Vector3 max{0.0f, 0.0f, 0.0f};
  ObjectLayer layer{0};
};

struct StaticPlane {
  math::Vector3 point{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  ObjectLayer layer{0};
};

struct StaticSphere {
  math::Vector3 center{0.0f, 0.0f, 0.0f};
  float radius_m{0.0f};
  ObjectLayer layer{0};
};

// Vertical (Y-axis) capsule; center is the geometric center and half_height_m
// is half the total height including the hemispherical caps.
struct StaticCapsule {
  math::Vector3 center{0.0f, 0.0f, 0.0f};
  float radius_m{0.0f};
  float half_height_m{0.0f};
  ObjectLayer layer{0};
};

enum class StaticGroundMode : uint8_t {
  kEnabled,
  kDisabled,
};

struct GroundHit {
  bool hit{false};
  math::Vector3 position{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  float distance_m{0.0f};
  ObjectLayer layer{0};
};

struct ShapeCastHit {
  bool hit{false};
  float fraction{1.0f};
  math::Vector3 position{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  float distance_m{0.0f};
  ObjectLayer layer{0};
};

struct RaycastHit {
  bool hit{false};
  float fraction{1.0f};
  math::Vector3 position{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  float distance_m{0.0f};
  ObjectLayer layer{0};
};

struct DepenetrationHit {
  bool hit{false};
  math::Vector3 offset{0.0f, 0.0f, 0.0f};
  math::Vector3 normal{0.0f, 1.0f, 0.0f};
  float depth_m{0.0f};
  ObjectLayer layer{0};
};

class PhysicsQuery {
 public:
  virtual ~PhysicsQuery() = default;

  [[nodiscard]] virtual auto GroundProbe(const GroundProbeQuery& query) const -> GroundHit = 0;
  [[nodiscard]] virtual auto Raycast(const RaycastQuery& query) const -> RaycastHit;
  [[nodiscard]] virtual auto CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit = 0;
  [[nodiscard]] virtual auto OverlapCapsule(const OverlapQuery& query) const -> bool = 0;
  [[nodiscard]] virtual auto DepenetrateCapsule(const OverlapQuery& query) const
      -> DepenetrationHit;
};

class NullPhysicsQuery final : public PhysicsQuery {
 public:
  [[nodiscard]] auto GroundProbe(const GroundProbeQuery& query) const -> GroundHit override;
  [[nodiscard]] auto CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit override;
  [[nodiscard]] auto OverlapCapsule(const OverlapQuery& query) const -> bool override;
};

class FlatPhysicsQuery final : public PhysicsQuery {
 public:
  explicit FlatPhysicsQuery(float ground_y = 0.0f) : ground_y_(ground_y) {}

  [[nodiscard]] auto GroundProbe(const GroundProbeQuery& query) const -> GroundHit override;
  [[nodiscard]] auto Raycast(const RaycastQuery& query) const -> RaycastHit override;
  [[nodiscard]] auto CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit override;
  [[nodiscard]] auto OverlapCapsule(const OverlapQuery& query) const -> bool override;
  [[nodiscard]] auto DepenetrateCapsule(const OverlapQuery& query) const
      -> DepenetrationHit override;

 private:
  float ground_y_{0.0f};
};

class StaticPhysicsQuery final : public PhysicsQuery {
 public:
  explicit StaticPhysicsQuery(float ground_y = 0.0f) : ground_y_(ground_y) {}
  explicit StaticPhysicsQuery(StaticGroundMode ground_mode, float ground_y = 0.0f)
      : has_flat_ground_(ground_mode == StaticGroundMode::kEnabled), ground_y_(ground_y) {}

  void AddBox(const StaticBox& box);
  void AddPlane(const StaticPlane& plane);
  void Clear();

  [[nodiscard]] auto GroundProbe(const GroundProbeQuery& query) const -> GroundHit override;
  [[nodiscard]] auto Raycast(const RaycastQuery& query) const -> RaycastHit override;
  [[nodiscard]] auto CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit override;
  [[nodiscard]] auto OverlapCapsule(const OverlapQuery& query) const -> bool override;
  [[nodiscard]] auto DepenetrateCapsule(const OverlapQuery& query) const
      -> DepenetrationHit override;

 private:
  bool has_flat_ground_{true};
  float ground_y_{0.0f};
  std::vector<StaticBox> boxes_;
  std::vector<StaticPlane> planes_;
};

class ChunkedPhysicsQuery final : public PhysicsQuery {
 public:
  void SetFallback(std::unique_ptr<PhysicsQuery> query);
  void AddChunk(const PhysicsQueryRegion& region, std::unique_ptr<PhysicsQuery> query);
  [[nodiscard]] auto ChunkCount() const -> std::size_t { return chunks_.size(); }

  [[nodiscard]] auto GroundProbe(const GroundProbeQuery& query) const -> GroundHit override;
  [[nodiscard]] auto Raycast(const RaycastQuery& query) const -> RaycastHit override;
  [[nodiscard]] auto CastCapsule(const CapsuleCastQuery& query) const -> ShapeCastHit override;
  [[nodiscard]] auto OverlapCapsule(const OverlapQuery& query) const -> bool override;
  [[nodiscard]] auto DepenetrateCapsule(const OverlapQuery& query) const
      -> DepenetrationHit override;

 private:
  struct Chunk {
    PhysicsQueryRegion region;
    std::unique_ptr<PhysicsQuery> query;
  };

  std::unique_ptr<PhysicsQuery> fallback_;
  std::vector<Chunk> chunks_;
};

}  // namespace atlas::physics

#endif  // ATLAS_LIB_PHYSICS_PHYSICS_QUERY_H_
