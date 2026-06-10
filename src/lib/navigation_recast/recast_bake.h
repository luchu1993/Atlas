#ifndef ATLAS_LIB_NAVIGATION_RECAST_RECAST_BAKE_H_
#define ATLAS_LIB_NAVIGATION_RECAST_RECAST_BAKE_H_

#include "foundation/error.h"
#include "navigation/nav_input.h"
#include "navigation/nav_params.h"

namespace atlas::nav {

struct RecastBakeReport {
  int input_triangles{0};
  int poly_count{0};
  int poly_vertex_count{0};
  float walkable_area_m2{0.0f};
};

// Owns the Detour tile blob (dtAlloc'd). Hand it to DetourNavQuery::Create
// (which takes ownership) or release it with FreeNavMeshData.
struct BakedNavMeshData {
  unsigned char* nav_data{nullptr};
  int nav_data_size{0};
  RecastBakeReport report;
};

// World-space detail-mesh triangles of the baked navmesh, for OBJ preview.
struct NavDebugMesh {
  std::vector<math::Vector3> vertices;
  std::vector<int32_t> indices;  // 3 per triangle
  RecastBakeReport report;
};

// Recast solo-mesh pipeline → one serialized Detour tile. Zero polygons is a
// hard error (the usual sign of inverted winding, bad bounds, or steep geometry).
[[nodiscard]] auto BuildNavMeshData(const NavInputGeometry& input, const NavBakeParams& params)
    -> Result<BakedNavMeshData>;

// Same pipeline, but returns the walkable surface as a triangle mesh instead of
// a Detour tile — used by `atlas_tool dump_nav`.
[[nodiscard]] auto BuildNavDebugMesh(const NavInputGeometry& input, const NavBakeParams& params)
    -> Result<NavDebugMesh>;

void FreeNavMeshData(BakedNavMeshData& data);

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_RECAST_RECAST_BAKE_H_
