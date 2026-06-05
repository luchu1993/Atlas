#include "navigation/nav_input.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <utility>

namespace atlas::nav {
namespace {

void PushTri(NavInputGeometry& geo, int32_t a, int32_t b, int32_t c, NavArea area) {
  geo.indices.push_back(a);
  geo.indices.push_back(b);
  geo.indices.push_back(c);
  geo.triangle_areas.push_back(area);
}

// 8 shared corners + 12 outward-wound triangles; the top face (y=max) carries a
// +Y normal so Recast classifies it walkable.
void EmitBox(NavInputGeometry& geo, const math::Vector3& mn, const math::Vector3& mx,
             NavArea area) {
  const auto base = static_cast<int32_t>(geo.vertices.size());
  geo.vertices.push_back({mn.x, mn.y, mn.z});  // 0
  geo.vertices.push_back({mx.x, mn.y, mn.z});  // 1
  geo.vertices.push_back({mx.x, mn.y, mx.z});  // 2
  geo.vertices.push_back({mn.x, mn.y, mx.z});  // 3
  geo.vertices.push_back({mn.x, mx.y, mn.z});  // 4
  geo.vertices.push_back({mx.x, mx.y, mn.z});  // 5
  geo.vertices.push_back({mx.x, mx.y, mx.z});  // 6
  geo.vertices.push_back({mn.x, mx.y, mx.z});  // 7
  static constexpr int32_t kFaces[12][3] = {
      {4, 7, 6}, {4, 6, 5},  // top (+Y)
      {0, 1, 2}, {0, 2, 3},  // bottom (-Y)
      {0, 4, 5}, {0, 5, 1},  // front (-Z)
      {3, 2, 6}, {3, 6, 7},  // back (+Z)
      {0, 3, 7}, {0, 7, 4},  // left (-X)
      {1, 5, 6}, {1, 6, 2},  // right (+X)
  };
  for (const auto& t : kFaces) PushTri(geo, base + t[0], base + t[1], base + t[2], area);
}

void EmitMesh(NavInputGeometry& geo, const physics::MeshGeometry& mesh, NavArea area) {
  const auto base = static_cast<int32_t>(geo.vertices.size());
  for (const auto& v : mesh.vertices) geo.vertices.push_back(v);
  for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
    PushTri(geo, base + static_cast<int32_t>(mesh.indices[i]),
            base + static_cast<int32_t>(mesh.indices[i + 1]),
            base + static_cast<int32_t>(mesh.indices[i + 2]), area);
  }
}

// Grid of two triangles per cell, top-facing (+Y). A cell touching any hole
// sample (FLT_MAX) is dropped so Recast leaves no walkable span over a hole.
void EmitHeightField(NavInputGeometry& geo, const physics::HeightFieldGeometry& hf, NavArea area) {
  const uint32_t n = hf.sample_count;
  const auto world_at = [&](uint32_t x, uint32_t z) -> math::Vector3 {
    const float h = hf.samples[std::size_t{z} * n + x];
    return {hf.origin.x + hf.scale.x * static_cast<float>(x), hf.origin.y + hf.scale.y * h,
            hf.origin.z + hf.scale.z * static_cast<float>(z)};
  };
  const auto is_hole = [&](uint32_t x, uint32_t z) -> bool {
    const float h = hf.samples[std::size_t{z} * n + x];
    return !std::isfinite(h) || h >= std::numeric_limits<float>::max();
  };
  for (uint32_t z = 0; z + 1 < n; ++z) {
    for (uint32_t x = 0; x + 1 < n; ++x) {
      if (is_hole(x, z) || is_hole(x + 1, z) || is_hole(x, z + 1) || is_hole(x + 1, z + 1)) {
        continue;
      }
      const auto base = static_cast<int32_t>(geo.vertices.size());
      geo.vertices.push_back(world_at(x, z));
      geo.vertices.push_back(world_at(x, z + 1));
      geo.vertices.push_back(world_at(x + 1, z + 1));
      geo.vertices.push_back(world_at(x + 1, z));
      PushTri(geo, base + 0, base + 1, base + 2, area);
      PushTri(geo, base + 0, base + 2, base + 3, area);
    }
  }
}

[[nodiscard]] auto AabbContains(const math::Vector3& lo, const math::Vector3& hi,
                                const math::Vector3& p) -> bool {
  return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y && p.z >= lo.z && p.z <= hi.z;
}

void ApplyAreaTags(NavInputGeometry& geo, const std::vector<NavOverrideVolume>& overrides) {
  const bool any = std::any_of(overrides.begin(), overrides.end(), [](const NavOverrideVolume& v) {
    return v.kind == NavOverrideKind::kAreaTag;
  });
  if (!any) return;
  for (std::size_t t = 0; t < geo.triangle_areas.size(); ++t) {
    const auto& a = geo.vertices[static_cast<std::size_t>(geo.indices[t * 3 + 0])];
    const auto& b = geo.vertices[static_cast<std::size_t>(geo.indices[t * 3 + 1])];
    const auto& c = geo.vertices[static_cast<std::size_t>(geo.indices[t * 3 + 2])];
    const math::Vector3 centroid = (a + b + c) * (1.0f / 3.0f);
    for (const auto& vol : overrides) {
      if (vol.kind != NavOverrideKind::kAreaTag) continue;
      const math::Vector3 lo{std::min(vol.min.x, vol.max.x), std::min(vol.min.y, vol.max.y),
                             std::min(vol.min.z, vol.max.z)};
      const math::Vector3 hi{std::max(vol.min.x, vol.max.x), std::max(vol.min.y, vol.max.y),
                             std::max(vol.min.z, vol.max.z)};
      if (AabbContains(lo, hi, centroid)) geo.triangle_areas[t] = vol.area;
    }
  }
}

void ComputeBounds(NavInputGeometry& geo, const NavParams& params) {
  if (params.bounds.has_explicit) {
    geo.bounds_min = params.bounds.min;
    geo.bounds_max = params.bounds.max;
    return;
  }
  if (geo.vertices.empty()) {
    geo.bounds_min = {0.0f, 0.0f, 0.0f};
    geo.bounds_max = {0.0f, 0.0f, 0.0f};
    return;
  }
  math::Vector3 lo = geo.vertices.front();
  math::Vector3 hi = geo.vertices.front();
  for (const auto& v : geo.vertices) {
    lo.x = std::min(lo.x, v.x);
    lo.y = std::min(lo.y, v.y);
    lo.z = std::min(lo.z, v.z);
    hi.x = std::max(hi.x, v.x);
    hi.y = std::max(hi.y, v.y);
    hi.z = std::max(hi.z, v.z);
  }
  const float m = params.bounds.margin_m;
  geo.bounds_min = {lo.x - m, lo.y - m, lo.z - m};
  geo.bounds_max = {hi.x + m, hi.y + m, hi.z + m};
}

}  // namespace

auto DeriveNavInput(const physics::CollisionAsset& asset, const NavParams& params)
    -> NavDeriveResult {
  NavDeriveResult result;
  auto& geo = result.geometry;
  auto& stats = result.stats;

  const auto role_for = [&](physics::ObjectLayer layer) -> NavRole {
    return layer < kNavLayerCount ? params.layer_roles[layer] : NavRole::kInclude;
  };
  const auto area_for = [](NavRole role) -> NavArea {
    return role == NavRole::kCarve ? NavArea::kNull : NavArea::kGround;
  };

  for (const auto& box : asset.boxes) {
    const auto role = role_for(box.layer);
    if (role == NavRole::kIgnore) continue;
    EmitBox(geo, box.min, box.max, area_for(role));
    ++stats.boxes;
  }
  for (const auto& mesh : asset.meshes) {
    const auto role = role_for(mesh.layer);
    if (role == NavRole::kIgnore) continue;
    EmitMesh(geo, mesh, area_for(role));
    ++stats.meshes;
  }
  for (const auto& hf : asset.heightfields) {
    const auto role = role_for(hf.layer);
    if (role == NavRole::kIgnore) continue;
    EmitHeightField(geo, hf, area_for(role));
    ++stats.heightfields;
  }

  if (!asset.convexes.empty()) {
    stats.skipped_convexes = asset.convexes.size();
    stats.warnings.push_back(std::format(
        "{} convex shape(s) skipped: nav v1 does not hull-triangulate point clouds",
        asset.convexes.size()));
  }
  if (!asset.spheres.empty()) {
    stats.skipped_spheres = asset.spheres.size();
    stats.warnings.push_back(
        std::format("{} sphere shape(s) skipped by nav v1", asset.spheres.size()));
  }
  if (!asset.capsules.empty()) {
    stats.skipped_capsules = asset.capsules.size();
    stats.warnings.push_back(
        std::format("{} capsule shape(s) skipped by nav v1", asset.capsules.size()));
  }
  if (!asset.planes.empty()) {
    stats.skipped_planes = asset.planes.size();
    stats.warnings.push_back(std::format(
        "{} infinite plane(s) skipped: author nav ground as a box/mesh in v1",
        asset.planes.size()));
  }

  for (const auto& vol : params.overrides) {
    if (vol.kind == NavOverrideKind::kForceWalkable) {
      EmitBox(geo, vol.min, vol.max, NavArea::kGround);
    } else if (vol.kind == NavOverrideKind::kForceBlocker) {
      EmitBox(geo, vol.min, vol.max, NavArea::kNull);
    }
  }

  if (params.bake.flip_winding) {
    for (std::size_t i = 0; i + 2 < geo.indices.size(); i += 3) {
      std::swap(geo.indices[i + 1], geo.indices[i + 2]);
    }
  }

  ApplyAreaTags(geo, params.overrides);
  stats.triangles = geo.TriangleCount();
  ComputeBounds(geo, params);
  return result;
}

}  // namespace atlas::nav
