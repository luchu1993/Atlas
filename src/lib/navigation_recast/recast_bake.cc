#include "navigation_recast/recast_bake.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4127 4244 4245 4267 4456 4701 5054)
#endif

#include "DetourAlloc.h"
#include "DetourNavMeshBuilder.h"
#include "Recast.h"

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cmath>
#include <cstring>
#include <format>
#include <vector>

namespace atlas::nav {
namespace {

[[nodiscard]] auto Invalid(std::string message) -> Error {
  return Error{ErrorCode::kInvalidArgument, std::move(message)};
}

// Frees every Recast intermediate on scope exit so each error path can just
// return without a manual cleanup ladder.
struct RcScratch {
  rcHeightfield* solid{nullptr};
  rcCompactHeightfield* chf{nullptr};
  rcContourSet* cset{nullptr};
  rcPolyMesh* pmesh{nullptr};
  rcPolyMeshDetail* dmesh{nullptr};
  ~RcScratch() {
    if (solid) rcFreeHeightField(solid);
    if (chf) rcFreeCompactHeightfield(chf);
    if (cset) rcFreeContourSet(cset);
    if (pmesh) rcFreePolyMesh(pmesh);
    if (dmesh) rcFreePolyMeshDetail(dmesh);
  }
};

}  // namespace

auto BuildNavMeshData(const NavInputGeometry& input, const NavBakeParams& params)
    -> Result<BakedNavMeshData> {
  const int nverts = static_cast<int>(input.vertices.size());
  const int ntris = static_cast<int>(input.TriangleCount());
  if (ntris == 0) return Invalid("nav bake: no input geometry (0 triangles)");

  const auto* verts = reinterpret_cast<const float*>(input.vertices.data());
  const auto* tris = reinterpret_cast<const int*>(input.indices.data());

  const float cs = params.cell_size_m;
  const float ch = params.cell_height_m;

  rcConfig cfg;
  std::memset(&cfg, 0, sizeof(cfg));
  cfg.cs = cs;
  cfg.ch = ch;
  cfg.walkableSlopeAngle = params.agent_max_slope_deg;
  cfg.walkableHeight = static_cast<int>(std::ceil(params.agent_height_m / ch));
  cfg.walkableClimb = static_cast<int>(std::floor(params.agent_max_climb_m / ch));
  cfg.walkableRadius = static_cast<int>(std::ceil(params.agent_radius_m / cs));
  cfg.maxEdgeLen = static_cast<int>(params.max_edge_len_m / cs);
  cfg.maxSimplificationError = params.max_simplification_error;
  cfg.minRegionArea = static_cast<int>(params.min_region_area_m2 / (cs * cs));
  cfg.mergeRegionArea = static_cast<int>(params.merge_region_area_m2 / (cs * cs));
  cfg.maxVertsPerPoly = 6;  // DT_VERTS_PER_POLYGON
  cfg.detailSampleDist =
      params.detail_sample_dist_m < cs * 0.9f ? 0.0f : params.detail_sample_dist_m;
  cfg.detailSampleMaxError = params.detail_sample_max_error_m;

  const float bmin[3] = {input.bounds_min.x, input.bounds_min.y, input.bounds_min.z};
  const float bmax[3] = {input.bounds_max.x, input.bounds_max.y, input.bounds_max.z};
  rcVcopy(cfg.bmin, bmin);
  rcVcopy(cfg.bmax, bmax);
  rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
  if (cfg.width <= 0 || cfg.height <= 0) return Invalid("nav bake: degenerate bake bounds");

  rcContext ctx(false);
  RcScratch scratch;

  scratch.solid = rcAllocHeightfield();
  if (scratch.solid == nullptr ||
      !rcCreateHeightfield(&ctx, *scratch.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs,
                           cfg.ch)) {
    return Invalid("nav bake: rcCreateHeightfield failed");
  }

  // Recast decides walkability by slope; combine that with our per-triangle
  // area so a carved triangle (area 0) stays non-walkable even on flat ground.
  std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
  rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts, nverts, tris, ntris, areas.data());
  for (int i = 0; i < ntris; ++i) {
    const auto mine = static_cast<unsigned char>(input.triangle_areas[static_cast<std::size_t>(i)]);
    areas[static_cast<std::size_t>(i)] = (areas[static_cast<std::size_t>(i)] != 0 && mine != 0)
                                             ? mine
                                             : static_cast<unsigned char>(0);
  }
  if (!rcRasterizeTriangles(&ctx, verts, nverts, tris, areas.data(), ntris, *scratch.solid,
                            cfg.walkableClimb)) {
    return Invalid("nav bake: rcRasterizeTriangles failed");
  }

  rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *scratch.solid);
  rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *scratch.solid);
  rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *scratch.solid);

  scratch.chf = rcAllocCompactHeightfield();
  if (scratch.chf == nullptr || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight,
                                                           cfg.walkableClimb, *scratch.solid,
                                                           *scratch.chf)) {
    return Invalid("nav bake: rcBuildCompactHeightfield failed");
  }
  if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *scratch.chf)) {
    return Invalid("nav bake: rcErodeWalkableArea failed");
  }

  if (params.partition == NavPartition::kMonotone) {
    if (!rcBuildRegionsMonotone(&ctx, *scratch.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
      return Invalid("nav bake: rcBuildRegionsMonotone failed");
    }
  } else {
    if (!rcBuildDistanceField(&ctx, *scratch.chf)) {
      return Invalid("nav bake: rcBuildDistanceField failed");
    }
    if (!rcBuildRegions(&ctx, *scratch.chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) {
      return Invalid("nav bake: rcBuildRegions failed");
    }
  }

  scratch.cset = rcAllocContourSet();
  if (scratch.cset == nullptr || !rcBuildContours(&ctx, *scratch.chf, cfg.maxSimplificationError,
                                                  cfg.maxEdgeLen, *scratch.cset)) {
    return Invalid("nav bake: rcBuildContours failed");
  }

  scratch.pmesh = rcAllocPolyMesh();
  if (scratch.pmesh == nullptr ||
      !rcBuildPolyMesh(&ctx, *scratch.cset, cfg.maxVertsPerPoly, *scratch.pmesh)) {
    return Invalid("nav bake: rcBuildPolyMesh failed");
  }
  if (scratch.pmesh->npolys == 0) {
    return Invalid(std::format(
        "nav bake: navmesh is empty (0 polygons from {} input triangles) — check winding "
        "(try flip_winding), bake bounds, or raise agent_max_slope_deg",
        ntris));
  }

  scratch.dmesh = rcAllocPolyMeshDetail();
  if (scratch.dmesh == nullptr ||
      !rcBuildPolyMeshDetail(&ctx, *scratch.pmesh, *scratch.chf, cfg.detailSampleDist,
                             cfg.detailSampleMaxError, *scratch.dmesh)) {
    return Invalid("nav bake: rcBuildPolyMeshDetail failed");
  }

  // Poly flags drive Detour include/exclude; bit 0 = walkable. The per-area
  // value stays in polyAreas for dtQueryFilter cost lookup.
  rcPolyMesh& pmesh = *scratch.pmesh;
  for (int i = 0; i < pmesh.npolys; ++i) {
    pmesh.flags[i] = pmesh.areas[i] != RC_NULL_AREA ? static_cast<unsigned short>(1) : 0;
  }

  dtNavMeshCreateParams dp;
  std::memset(&dp, 0, sizeof(dp));
  dp.verts = pmesh.verts;
  dp.vertCount = pmesh.nverts;
  dp.polys = pmesh.polys;
  dp.polyAreas = pmesh.areas;
  dp.polyFlags = pmesh.flags;
  dp.polyCount = pmesh.npolys;
  dp.nvp = pmesh.nvp;
  dp.detailMeshes = scratch.dmesh->meshes;
  dp.detailVerts = scratch.dmesh->verts;
  dp.detailVertsCount = scratch.dmesh->nverts;
  dp.detailTris = scratch.dmesh->tris;
  dp.detailTriCount = scratch.dmesh->ntris;
  dp.walkableHeight = params.agent_height_m;
  dp.walkableRadius = params.agent_radius_m;
  dp.walkableClimb = params.agent_max_climb_m;
  rcVcopy(dp.bmin, pmesh.bmin);
  rcVcopy(dp.bmax, pmesh.bmax);
  dp.cs = cfg.cs;
  dp.ch = cfg.ch;
  dp.buildBvTree = true;

  unsigned char* nav_data = nullptr;
  int nav_data_size = 0;
  if (!dtCreateNavMeshData(&dp, &nav_data, &nav_data_size)) {
    return Invalid("nav bake: dtCreateNavMeshData failed");
  }

  BakedNavMeshData out;
  out.nav_data = nav_data;
  out.nav_data_size = nav_data_size;
  out.report.input_triangles = ntris;
  out.report.poly_count = pmesh.npolys;
  out.report.poly_vertex_count = pmesh.nverts;
  return out;
}

void FreeNavMeshData(BakedNavMeshData& data) {
  if (data.nav_data != nullptr) {
    dtFree(data.nav_data);
    data.nav_data = nullptr;
    data.nav_data_size = 0;
  }
}

}  // namespace atlas::nav
