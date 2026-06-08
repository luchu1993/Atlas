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
};

// Owns the Detour tile blob (dtAlloc'd). Hand it to DetourNavQuery::Create
// (which takes ownership) or release it with FreeNavMeshData.
struct BakedNavMeshData {
  unsigned char* nav_data{nullptr};
  int nav_data_size{0};
  RecastBakeReport report;
};

// Runs the Recast solo-mesh pipeline and serializes a single Detour tile.
// Fails with a clear error when the bake yields zero polygons (the usual sign
// of inverted winding, bad bounds, or too-steep geometry).
[[nodiscard]] auto BuildNavMeshData(const NavInputGeometry& input, const NavBakeParams& params)
    -> Result<BakedNavMeshData>;

void FreeNavMeshData(BakedNavMeshData& data);

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_RECAST_RECAST_BAKE_H_
