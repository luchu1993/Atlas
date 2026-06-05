#include <gtest/gtest.h>

#include "navigation/nav_params.h"

namespace atlas::nav {
namespace {

constexpr const char* kValid = R"({
  "version": 1,
  "coordinate_system": "x_right_y_up_z_forward_meters",
  "source_hash": "unit",
  "bake": {"cell_size": 0.25, "agent_radius": 0.5, "partition": "monotone", "flip_winding": true},
  "bounds": {"min": [-10, -1, -10], "max": [10, 5, 10], "margin": 2.0},
  "layer_roles": [{"layer": 13, "role": "carve"}, {"layer": 7, "role": "ignore"}],
  "overrides": [{"kind": "area_tag", "min": [0, 0, 0], "max": [1, 1, 1], "area": "water"}]
})";

TEST(NavParams, LoadsValid) {
  auto params = LoadNavParamsFromJson(kValid);
  ASSERT_TRUE(params.HasValue()) << params.Error().Message();
  EXPECT_EQ(params->source_hash, "unit");
  EXPECT_FLOAT_EQ(params->bake.cell_size_m, 0.25f);
  EXPECT_FLOAT_EQ(params->bake.agent_radius_m, 0.5f);
  EXPECT_EQ(params->bake.partition, NavPartition::kMonotone);
  EXPECT_TRUE(params->bake.flip_winding);
  EXPECT_FLOAT_EQ(params->bake.agent_height_m, 1.80f);  // untouched default
  EXPECT_TRUE(params->bounds.has_explicit);
  EXPECT_FLOAT_EQ(params->bounds.margin_m, 2.0f);
  EXPECT_EQ(params->layer_roles[13], NavRole::kCarve);
  EXPECT_EQ(params->layer_roles[7], NavRole::kIgnore);
  EXPECT_EQ(params->layer_roles[0], NavRole::kInclude);  // default
  ASSERT_EQ(params->overrides.size(), 1u);
  EXPECT_EQ(params->overrides[0].kind, NavOverrideKind::kAreaTag);
  EXPECT_EQ(params->overrides[0].area, NavArea::kWater);
}

TEST(NavParams, DefaultsWhenBakeOmitted) {
  constexpr const char* json =
      R"({"version":1,"coordinate_system":"x_right_y_up_z_forward_meters","source_hash":"u"})";
  auto params = LoadNavParamsFromJson(json);
  ASSERT_TRUE(params.HasValue()) << params.Error().Message();
  EXPECT_FLOAT_EQ(params->bake.cell_size_m, 0.20f);
  EXPECT_FALSE(params->bounds.has_explicit);
  EXPECT_EQ(params->layer_roles[5], NavRole::kInclude);
}

TEST(NavParams, RejectsBadCoordinateSystem) {
  constexpr const char* json =
      R"({"version":1,"coordinate_system":"y_up","source_hash":"u"})";
  EXPECT_FALSE(LoadNavParamsFromJson(json).HasValue());
}

TEST(NavParams, RejectsNonPositiveAgentRadius) {
  constexpr const char* json =
      R"({"version":1,"coordinate_system":"x_right_y_up_z_forward_meters","source_hash":"u",)"
      R"("bake":{"agent_radius":0}})";
  EXPECT_FALSE(LoadNavParamsFromJson(json).HasValue());
}

TEST(NavParams, RejectsSlopeOver90) {
  constexpr const char* json =
      R"({"version":1,"coordinate_system":"x_right_y_up_z_forward_meters","source_hash":"u",)"
      R"("bake":{"agent_max_slope_deg":120}})";
  EXPECT_FALSE(LoadNavParamsFromJson(json).HasValue());
}

TEST(NavParams, RejectsUnsupportedVersion) {
  constexpr const char* json =
      R"({"version":99,"coordinate_system":"x_right_y_up_z_forward_meters","source_hash":"u"})";
  EXPECT_FALSE(LoadNavParamsFromJson(json).HasValue());
}

TEST(NavParams, ValidateRejectsInvertedBounds) {
  NavParams params;
  params.source_hash = "u";
  params.bounds.has_explicit = true;
  params.bounds.min = {1, 1, 1};
  params.bounds.max = {0, 0, 0};
  EXPECT_FALSE(ValidateNavParams(params).HasValue());
}

}  // namespace
}  // namespace atlas::nav
