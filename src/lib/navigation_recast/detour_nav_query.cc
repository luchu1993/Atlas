#include "navigation_recast/detour_nav_query.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4244 4267 4456 4701 5054)
#endif

#include "DetourAlloc.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshQuery.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <array>
#include <cstring>

namespace atlas::nav {
namespace {

constexpr int kMaxPathPolys = 256;

void FillFilter(const NavQueryFilter& in, dtQueryFilter& out) {
  out.setIncludeFlags(in.include_flags);
  out.setExcludeFlags(in.exclude_flags);
  for (int i = 0; i < static_cast<int>(kNavMaxAreas); ++i) {
    out.setAreaCost(i, in.area_cost[static_cast<std::size_t>(i)]);
  }
}

[[nodiscard]] auto Vec(const float* p) -> math::Vector3 { return {p[0], p[1], p[2]}; }

}  // namespace

auto DetourNavQuery::Create(unsigned char* nav_data, int nav_data_size, uint32_t max_nodes,
                            float vertical_extent_m) -> Result<std::unique_ptr<DetourNavQuery>> {
  dtNavMesh* mesh = dtAllocNavMesh();
  if (mesh == nullptr) {
    dtFree(nav_data);
    return Error{ErrorCode::kOutOfMemory, "nav: dtAllocNavMesh failed"};
  }
  // DT_TILE_FREE_DATA: on success the mesh owns nav_data; on failure no tile
  // was added, so we still own it and must free it.
  if (dtStatusFailed(mesh->init(nav_data, nav_data_size, DT_TILE_FREE_DATA))) {
    dtFreeNavMesh(mesh);
    dtFree(nav_data);
    return Error{ErrorCode::kInvalidArgument, "nav: dtNavMesh::init failed"};
  }
  dtNavMeshQuery* query = dtAllocNavMeshQuery();
  if (query == nullptr) {
    dtFreeNavMesh(mesh);
    return Error{ErrorCode::kOutOfMemory, "nav: dtAllocNavMeshQuery failed"};
  }
  if (dtStatusFailed(query->init(mesh, static_cast<int>(max_nodes)))) {
    dtFreeNavMeshQuery(query);
    dtFreeNavMesh(mesh);
    return Error{ErrorCode::kInvalidArgument, "nav: dtNavMeshQuery::init failed"};
  }

  auto self = std::unique_ptr<DetourNavQuery>(new DetourNavQuery());
  self->mesh_ = mesh;
  self->query_ = query;
  self->ext_[1] = vertical_extent_m;
  return self;
}

DetourNavQuery::~DetourNavQuery() {
  if (query_ != nullptr) dtFreeNavMeshQuery(query_);
  if (mesh_ != nullptr) dtFreeNavMesh(mesh_);  // frees the owned tile data
}

auto DetourNavQuery::FindPath(const math::Vector3& start, const math::Vector3& end,
                              const NavQueryFilter& filter) const -> NavPath {
  NavPath out;  // kEmpty by default
  dtQueryFilter dt_filter;
  FillFilter(filter, dt_filter);

  const float s[3] = {start.x, start.y, start.z};
  const float e[3] = {end.x, end.y, end.z};
  dtPolyRef start_ref = 0;
  dtPolyRef end_ref = 0;
  float start_pt[3];
  float end_pt[3];
  if (dtStatusFailed(query_->findNearestPoly(s, ext_, &dt_filter, &start_ref, start_pt)) ||
      dtStatusFailed(query_->findNearestPoly(e, ext_, &dt_filter, &end_ref, end_pt)) ||
      start_ref == 0 || end_ref == 0) {
    return out;
  }

  std::array<dtPolyRef, kMaxPathPolys> polys{};
  int npolys = 0;
  if (dtStatusFailed(query_->findPath(start_ref, end_ref, start_pt, end_pt, &dt_filter,
                                      polys.data(), &npolys, kMaxPathPolys)) ||
      npolys == 0) {
    return out;
  }

  const bool reached = polys[static_cast<std::size_t>(npolys - 1)] == end_ref;
  float target[3];
  if (reached) {
    std::memcpy(target, end_pt, sizeof(target));
  } else {
    // Goal poly unreachable: steer to the closest point on the last poly.
    query_->closestPointOnPoly(polys[static_cast<std::size_t>(npolys - 1)], end_pt, target, nullptr);
  }

  std::array<float, kMaxPathPolys * 3> straight{};
  std::array<unsigned char, kMaxPathPolys> straight_flags{};
  std::array<dtPolyRef, kMaxPathPolys> straight_refs{};
  int nstraight = 0;
  if (dtStatusFailed(query_->findStraightPath(start_pt, target, polys.data(), npolys,
                                              straight.data(), straight_flags.data(),
                                              straight_refs.data(), &nstraight, kMaxPathPolys)) ||
      nstraight == 0) {
    return out;
  }

  out.waypoints.reserve(static_cast<std::size_t>(nstraight));
  float length = 0.0f;
  for (int i = 0; i < nstraight; ++i) {
    const math::Vector3 p = Vec(&straight[static_cast<std::size_t>(i) * 3]);
    if (i > 0) length += p.Distance(out.waypoints.back());
    out.waypoints.push_back(p);
  }
  out.length_m = length;
  out.status = reached ? NavPathStatus::kReached : NavPathStatus::kPartial;
  return out;
}

auto DetourNavQuery::NearestPoint(const math::Vector3& point, const math::Vector3& half_extents,
                                  const NavQueryFilter& filter) const -> NavPoint {
  dtQueryFilter dt_filter;
  FillFilter(filter, dt_filter);
  const float p[3] = {point.x, point.y, point.z};
  const float ext[3] = {half_extents.x, half_extents.y, half_extents.z};
  dtPolyRef ref = 0;
  float nearest[3];
  NavPoint out;
  if (dtStatusFailed(query_->findNearestPoly(p, ext, &dt_filter, &ref, nearest)) || ref == 0) {
    return out;  // on_mesh == false
  }
  out.on_mesh = true;
  out.position = Vec(nearest);
  return out;
}

auto DetourNavQuery::Raycast(const math::Vector3& start, const math::Vector3& end,
                             const NavQueryFilter& filter) const -> NavRaycastHit {
  NavRaycastHit out;
  dtQueryFilter dt_filter;
  FillFilter(filter, dt_filter);
  const float s[3] = {start.x, start.y, start.z};
  const float e[3] = {end.x, end.y, end.z};
  dtPolyRef start_ref = 0;
  float start_pt[3];
  if (dtStatusFailed(query_->findNearestPoly(s, ext_, &dt_filter, &start_ref, start_pt)) ||
      start_ref == 0) {
    return out;  // no start poly → treat as unblocked
  }

  float t = 0.0f;
  float hit_normal[3] = {0.0f, 1.0f, 0.0f};
  std::array<dtPolyRef, kMaxPathPolys> path{};
  int npath = 0;
  if (dtStatusFailed(query_->raycast(start_ref, start_pt, e, &dt_filter, &t, hit_normal,
                                     path.data(), &npath, kMaxPathPolys))) {
    return out;
  }
  // t >= 1 (Detour reports a large value) means the segment stayed on the mesh.
  const math::Vector3 a = Vec(start_pt);
  const math::Vector3 b = Vec(e);
  if (t >= 1.0f) {
    out.blocked = false;
    out.t = 1.0f;
    out.position = b;
  } else {
    out.blocked = true;
    out.t = t;
    out.position = a + (b - a) * t;
    out.normal = Vec(hit_normal);
  }
  return out;
}

}  // namespace atlas::nav
