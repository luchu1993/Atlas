#ifndef ATLAS_LIB_NAVIGATION_NAV_PARAMS_H_
#define ATLAS_LIB_NAVIGATION_NAV_PARAMS_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "foundation/error.h"
#include "math/vector3.h"
#include "navigation/nav_query.h"

namespace atlas::nav {

inline constexpr uint32_t kNavParamsVersion = 1;
inline constexpr std::string_view kNavCoordinateSystem = "x_right_y_up_z_forward_meters";
inline constexpr std::size_t kNavLayerCount = 32;  // matches collision layer range [0,31]

enum class NavPartition : uint8_t { kWatershed, kMonotone, kLayers };

// How a collision object feeds the bake. Walkability itself is decided by Recast
// from slope + agent clearance; this only gates inclusion.
enum class NavRole : uint8_t { kInclude, kCarve, kIgnore };

// Lengths in meters, angles in degrees. Defaults target an OW-ish human agent
// at ~0.2 m voxels.
struct NavBakeParams {
  float cell_size_m{0.20f};
  float cell_height_m{0.10f};
  float agent_radius_m{0.40f};
  float agent_height_m{1.80f};
  float agent_max_climb_m{0.45f};
  float agent_max_slope_deg{50.0f};
  float min_region_area_m2{1.0f};
  float merge_region_area_m2{5.0f};
  float max_edge_len_m{12.0f};
  float max_simplification_error{1.3f};
  float detail_sample_dist_m{6.0f};
  float detail_sample_max_error_m{1.0f};
  float vertical_query_extent_m{1.0f};  // NearestPoint search half-height
  uint32_t max_search_nodes{2048};      // Detour node-pool budget per query
  NavPartition partition{NavPartition::kWatershed};
  bool flip_winding{false};
};

// Optional explicit bake bounds; when absent the bake derives them from the
// input geometry AABB grown by `margin_m`.
struct NavBounds {
  bool has_explicit{false};
  math::Vector3 min{0.0f, 0.0f, 0.0f};
  math::Vector3 max{0.0f, 0.0f, 0.0f};
  float margin_m{1.0f};
};

enum class NavOverrideKind : uint8_t { kForceWalkable, kForceBlocker, kAreaTag };

// World-space AABB authored next to the scene; spatial, so it survives a
// collision re-export without a stable object id.
struct NavOverrideVolume {
  NavOverrideKind kind{NavOverrideKind::kAreaTag};
  math::Vector3 min{0.0f, 0.0f, 0.0f};
  math::Vector3 max{0.0f, 0.0f, 0.0f};
  NavArea area{NavArea::kGround};
};

struct NavParams {
  NavParams() { layer_roles.fill(NavRole::kInclude); }

  uint32_t version{kNavParamsVersion};
  std::string coordinate_system{std::string(kNavCoordinateSystem)};
  std::string source_hash;
  NavBakeParams bake;
  NavBounds bounds;
  std::array<NavRole, kNavLayerCount> layer_roles;
  std::vector<NavOverrideVolume> overrides;
};

[[nodiscard]] auto ValidateNavParams(const NavParams& params) -> Result<void>;
[[nodiscard]] auto LoadNavParamsFromJson(std::string_view json) -> Result<NavParams>;
[[nodiscard]] auto LoadNavParamsFromFile(const std::filesystem::path& path) -> Result<NavParams>;

}  // namespace atlas::nav

#endif  // ATLAS_LIB_NAVIGATION_NAV_PARAMS_H_
