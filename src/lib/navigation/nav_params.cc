#include "navigation/nav_params.h"

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 5054)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <utility>

#include "platform/filesystem.h"

namespace atlas::nav {
namespace {

[[nodiscard]] auto Invalid(std::string message) -> Error {
  return Error{ErrorCode::kInvalidArgument, std::move(message)};
}

[[nodiscard]] auto Member(const rapidjson::Value& v, std::string_view member)
    -> const rapidjson::Value* {
  const std::string name(member);
  auto it = v.FindMember(name.c_str());
  return it == v.MemberEnd() ? nullptr : &it->value;
}

[[nodiscard]] auto RequiredString(const rapidjson::Value& v, std::string_view path,
                                  std::string_view member) -> Result<std::string> {
  if (!v.IsObject()) return Invalid(std::format("{} must be an object", path));
  const auto* mv = Member(v, member);
  if (mv == nullptr) return Invalid(std::format("{}.{} is required", path, member));
  if (!mv->IsString()) return Invalid(std::format("{}.{} must be a string", path, member));
  return std::string(mv->GetString(), mv->GetStringLength());
}

[[nodiscard]] auto RequiredUint(const rapidjson::Value& v, std::string_view path,
                                std::string_view member) -> Result<uint32_t> {
  if (!v.IsObject()) return Invalid(std::format("{} must be an object", path));
  const auto* mv = Member(v, member);
  if (mv == nullptr) return Invalid(std::format("{}.{} is required", path, member));
  if (!mv->IsUint()) return Invalid(std::format("{}.{} must be an unsigned integer", path, member));
  return mv->GetUint();
}

[[nodiscard]] auto OptionalNumber(const rapidjson::Value& v, std::string_view path,
                                  std::string_view member, float fallback) -> Result<float> {
  const auto* mv = Member(v, member);
  if (mv == nullptr) return fallback;
  if (!mv->IsNumber()) return Invalid(std::format("{}.{} must be a number", path, member));
  const double d = mv->GetDouble();
  if (!std::isfinite(d)) return Invalid(std::format("{}.{} must be finite", path, member));
  return static_cast<float>(d);
}

[[nodiscard]] auto OptionalUint(const rapidjson::Value& v, std::string_view path,
                                std::string_view member, uint32_t fallback) -> Result<uint32_t> {
  const auto* mv = Member(v, member);
  if (mv == nullptr) return fallback;
  if (!mv->IsUint()) return Invalid(std::format("{}.{} must be an unsigned integer", path, member));
  return mv->GetUint();
}

[[nodiscard]] auto OptionalBool(const rapidjson::Value& v, std::string_view path,
                                std::string_view member, bool fallback) -> Result<bool> {
  const auto* mv = Member(v, member);
  if (mv == nullptr) return fallback;
  if (!mv->IsBool()) return Invalid(std::format("{}.{} must be a boolean", path, member));
  return mv->GetBool();
}

[[nodiscard]] auto RequiredVec3(const rapidjson::Value& v, std::string_view path,
                                std::string_view member) -> Result<math::Vector3> {
  if (!v.IsObject()) return Invalid(std::format("{} must be an object", path));
  const auto* mv = Member(v, member);
  if (mv == nullptr) return Invalid(std::format("{}.{} is required", path, member));
  if (!mv->IsArray() || mv->Size() != 3) {
    return Invalid(std::format("{}.{} must be a 3-number array", path, member));
  }
  math::Vector3 out;
  for (rapidjson::SizeType i = 0; i < 3; ++i) {
    if (!(*mv)[i].IsNumber()) {
      return Invalid(std::format("{}.{}[{}] must be a number", path, member, i));
    }
    const double d = (*mv)[i].GetDouble();
    if (!std::isfinite(d)) return Invalid(std::format("{}.{}[{}] must be finite", path, member, i));
    out[static_cast<std::size_t>(i)] = static_cast<float>(d);
  }
  return out;
}

[[nodiscard]] auto ParsePartition(const std::string& s, NavPartition& out) -> bool {
  if (s == "watershed") {
    out = NavPartition::kWatershed;
  } else if (s == "monotone") {
    out = NavPartition::kMonotone;
  } else if (s == "layers") {
    out = NavPartition::kLayers;
  } else {
    return false;
  }
  return true;
}

[[nodiscard]] auto ParseArea(const std::string& s, NavArea& out) -> bool {
  if (s == "null") {
    out = NavArea::kNull;
  } else if (s == "ground") {
    out = NavArea::kGround;
  } else if (s == "water") {
    out = NavArea::kWater;
  } else if (s == "jump") {
    out = NavArea::kJump;
  } else if (s == "door") {
    out = NavArea::kDoor;
  } else {
    return false;
  }
  return true;
}

[[nodiscard]] auto ParseRole(const std::string& s, NavRole& out) -> bool {
  if (s == "include") {
    out = NavRole::kInclude;
  } else if (s == "carve") {
    out = NavRole::kCarve;
  } else if (s == "ignore") {
    out = NavRole::kIgnore;
  } else {
    return false;
  }
  return true;
}

[[nodiscard]] auto ParseBake(const rapidjson::Value& obj, NavBakeParams& bake) -> std::optional<Error> {
  if (!obj.IsObject()) return Invalid("bake must be an object");
  auto num = [&](std::string_view key, float& dst) -> std::optional<Error> {
    auto r = OptionalNumber(obj, "bake", key, dst);
    if (!r) return r.Error();
    dst = *r;
    return std::nullopt;
  };
  if (auto e = num("cell_size", bake.cell_size_m)) return e;
  if (auto e = num("cell_height", bake.cell_height_m)) return e;
  if (auto e = num("agent_radius", bake.agent_radius_m)) return e;
  if (auto e = num("agent_height", bake.agent_height_m)) return e;
  if (auto e = num("agent_max_climb", bake.agent_max_climb_m)) return e;
  if (auto e = num("agent_max_slope_deg", bake.agent_max_slope_deg)) return e;
  if (auto e = num("min_region_area", bake.min_region_area_m2)) return e;
  if (auto e = num("merge_region_area", bake.merge_region_area_m2)) return e;
  if (auto e = num("max_edge_len", bake.max_edge_len_m)) return e;
  if (auto e = num("max_simplification_error", bake.max_simplification_error)) return e;
  if (auto e = num("detail_sample_dist", bake.detail_sample_dist_m)) return e;
  if (auto e = num("detail_sample_max_error", bake.detail_sample_max_error_m)) return e;
  if (auto e = num("vertical_query_extent", bake.vertical_query_extent_m)) return e;

  auto nodes = OptionalUint(obj, "bake", "max_search_nodes", bake.max_search_nodes);
  if (!nodes) return nodes.Error();
  bake.max_search_nodes = *nodes;

  auto flip = OptionalBool(obj, "bake", "flip_winding", bake.flip_winding);
  if (!flip) return flip.Error();
  bake.flip_winding = *flip;

  if (const auto* p = Member(obj, "partition")) {
    if (!p->IsString()) return Invalid("bake.partition must be a string");
    if (!ParsePartition(std::string(p->GetString(), p->GetStringLength()), bake.partition)) {
      return Invalid("bake.partition must be watershed/monotone/layers");
    }
  }
  return std::nullopt;
}

[[nodiscard]] auto ParseBounds(const rapidjson::Value& obj, NavBounds& bounds) -> std::optional<Error> {
  if (!obj.IsObject()) return Invalid("bounds must be an object");
  auto mn = RequiredVec3(obj, "bounds", "min");
  if (!mn) return mn.Error();
  auto mx = RequiredVec3(obj, "bounds", "max");
  if (!mx) return mx.Error();
  auto margin = OptionalNumber(obj, "bounds", "margin", bounds.margin_m);
  if (!margin) return margin.Error();
  bounds.has_explicit = true;
  bounds.min = *mn;
  bounds.max = *mx;
  bounds.margin_m = *margin;
  return std::nullopt;
}

[[nodiscard]] auto ParseLayerRoles(const rapidjson::Value& arr, NavParams& params)
    -> std::optional<Error> {
  if (!arr.IsArray()) return Invalid("layer_roles must be an array");
  for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
    const auto path = std::format("layer_roles[{}]", i);
    auto layer = RequiredUint(arr[i], path, "layer");
    if (!layer) return layer.Error();
    if (*layer >= kNavLayerCount) {
      return Invalid(std::format("{}.layer must be in [0, {}]", path, kNavLayerCount - 1));
    }
    auto role_str = RequiredString(arr[i], path, "role");
    if (!role_str) return role_str.Error();
    NavRole role{};
    if (!ParseRole(*role_str, role)) {
      return Invalid(std::format("{}.role must be include/carve/ignore", path));
    }
    params.layer_roles[*layer] = role;
  }
  return std::nullopt;
}

[[nodiscard]] auto ParseOverrides(const rapidjson::Value& arr, NavParams& params)
    -> std::optional<Error> {
  if (!arr.IsArray()) return Invalid("overrides must be an array");
  for (rapidjson::SizeType i = 0; i < arr.Size(); ++i) {
    const auto path = std::format("overrides[{}]", i);
    auto kind_str = RequiredString(arr[i], path, "kind");
    if (!kind_str) return kind_str.Error();
    NavOverrideVolume vol;
    if (*kind_str == "force_walkable") {
      vol.kind = NavOverrideKind::kForceWalkable;
    } else if (*kind_str == "force_blocker") {
      vol.kind = NavOverrideKind::kForceBlocker;
    } else if (*kind_str == "area_tag") {
      vol.kind = NavOverrideKind::kAreaTag;
    } else {
      return Invalid(std::format("{}.kind must be force_walkable/force_blocker/area_tag", path));
    }
    auto mn = RequiredVec3(arr[i], path, "min");
    if (!mn) return mn.Error();
    auto mx = RequiredVec3(arr[i], path, "max");
    if (!mx) return mx.Error();
    vol.min = *mn;
    vol.max = *mx;
    if (const auto* a = Member(arr[i], "area")) {
      if (!a->IsString()) return Invalid(std::format("{}.area must be a string", path));
      if (!ParseArea(std::string(a->GetString(), a->GetStringLength()), vol.area)) {
        return Invalid(std::format("{}.area must be null/ground/water/jump/door", path));
      }
    }
    params.overrides.push_back(vol);
  }
  return std::nullopt;
}

}  // namespace

auto ValidateNavParams(const NavParams& params) -> Result<void> {
  if (params.version != kNavParamsVersion) {
    return Error{ErrorCode::kNotSupported,
                 std::format("nav params version {} is not supported (expected {})",
                             params.version, kNavParamsVersion)};
  }
  if (params.coordinate_system != kNavCoordinateSystem) {
    return Invalid(std::format("coordinate_system unsupported: {}", params.coordinate_system));
  }
  if (params.source_hash.empty()) return Invalid("source_hash must be non-empty");

  const auto& b = params.bake;
  if (b.cell_size_m <= 0.0f) return Invalid("bake.cell_size must be positive");
  if (b.cell_height_m <= 0.0f) return Invalid("bake.cell_height must be positive");
  if (b.agent_radius_m <= 0.0f) return Invalid("bake.agent_radius must be positive");
  if (b.agent_height_m <= 0.0f) return Invalid("bake.agent_height must be positive");
  if (b.agent_max_climb_m < 0.0f) return Invalid("bake.agent_max_climb must be >= 0");
  if (b.agent_max_climb_m >= b.agent_height_m) {
    return Invalid("bake.agent_max_climb must be < agent_height");
  }
  if (b.agent_max_slope_deg <= 0.0f || b.agent_max_slope_deg > 90.0f) {
    return Invalid("bake.agent_max_slope_deg must be in (0, 90]");
  }
  if (b.max_search_nodes == 0) return Invalid("bake.max_search_nodes must be positive");
  if (b.vertical_query_extent_m <= 0.0f) return Invalid("bake.vertical_query_extent must be positive");

  if (params.bounds.has_explicit) {
    const auto& mn = params.bounds.min;
    const auto& mx = params.bounds.max;
    if (mx.x <= mn.x || mx.y <= mn.y || mx.z <= mn.z) {
      return Invalid("bounds.max must be greater than min on every axis");
    }
  }
  return {};
}

auto LoadNavParamsFromJson(std::string_view json) -> Result<NavParams> {
  rapidjson::Document doc;
  doc.Parse(json.data(), json.size());
  if (doc.HasParseError()) {
    return Error{ErrorCode::kInvalidArgument,
                 std::format("nav params JSON parse error: {} at offset {}",
                             rapidjson::GetParseError_En(doc.GetParseError()),
                             doc.GetErrorOffset())};
  }
  if (!doc.IsObject()) return Invalid("root must be an object");

  auto version = RequiredUint(doc, "root", "version");
  if (!version) return version.Error();
  auto coordinate_system = RequiredString(doc, "root", "coordinate_system");
  if (!coordinate_system) return coordinate_system.Error();
  auto source_hash = RequiredString(doc, "root", "source_hash");
  if (!source_hash) return source_hash.Error();

  NavParams params;
  params.version = *version;
  params.coordinate_system = *coordinate_system;
  params.source_hash = *source_hash;

  if (const auto* bake = Member(doc, "bake")) {
    if (auto e = ParseBake(*bake, params.bake)) return *e;
  }
  if (const auto* bounds = Member(doc, "bounds")) {
    if (auto e = ParseBounds(*bounds, params.bounds)) return *e;
  }
  if (const auto* roles = Member(doc, "layer_roles")) {
    if (auto e = ParseLayerRoles(*roles, params)) return *e;
  }
  if (const auto* overrides = Member(doc, "overrides")) {
    if (auto e = ParseOverrides(*overrides, params)) return *e;
  }

  if (auto ok = ValidateNavParams(params); !ok) return ok.Error();
  return params;
}

auto LoadNavParamsFromFile(const std::filesystem::path& path) -> Result<NavParams> {
  auto text = fs::ReadTextFile(path);
  if (!text) return text.Error();
  return LoadNavParamsFromJson(*text);
}

}  // namespace atlas::nav
