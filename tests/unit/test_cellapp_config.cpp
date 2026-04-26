#include <gtest/gtest.h>

#include "cellapp_config.h"
#include "serialization/data_section.h"
#include "server/server_app_option.h"

using namespace atlas;

namespace {

// Helper: apply an empty DataSection so the three CellAppConfig statics
// snap back to their registered defaults. Used both at fixture teardown
// and as a hermetic starting point.
void ResetCellAppConfigToDefaults() {
  auto empty = DataSection::FromJsonString("{}");
  ASSERT_TRUE(empty.HasValue());
  ServerAppOptionBase::ApplyAll(*(*empty)->Root());
}

}  // namespace

class CellAppConfigTest : public ::testing::Test {
 protected:
  void SetUp() override { ResetCellAppConfigToDefaults(); }
  void TearDown() override { ResetCellAppConfigToDefaults(); }
};

TEST_F(CellAppConfigTest, DefaultsMatchExpectedValues) {
  EXPECT_FLOAT_EQ(CellAppConfig::DefaultAoIRadius(), 500.f);
  EXPECT_FLOAT_EQ(CellAppConfig::DefaultAoIHysteresis(), 5.f);
  EXPECT_FLOAT_EQ(CellAppConfig::MaxAoIRadius(), 500.f);
  EXPECT_EQ(CellAppConfig::WitnessTotalOutboundBudgetBytes(), 409600u);
  EXPECT_EQ(CellAppConfig::WitnessMinPerObserverBudgetBytes(), 1024u);
  EXPECT_EQ(CellAppConfig::WitnessMaxPerObserverBudgetBytes(), 16384u);
  EXPECT_EQ(CellAppConfig::WitnessMaxPeersPerTick(), 64u);
}

TEST_F(CellAppConfigTest, WitnessBudgetOverride) {
  auto cfg = DataSection::FromJsonString(R"({
    "witness_min_per_observer_budget_bytes": 2048,
    "witness_max_per_observer_budget_bytes": 32768
  })");
  ASSERT_TRUE(cfg.HasValue());
  ServerAppOptionBase::ApplyAll(*(*cfg)->Root());
  EXPECT_EQ(CellAppConfig::WitnessMinPerObserverBudgetBytes(), 2048u);
  EXPECT_EQ(CellAppConfig::WitnessMaxPerObserverBudgetBytes(), 32768u);
}

TEST_F(CellAppConfigTest, LoadsAllThreeKeysFromJson) {
  auto cfg = DataSection::FromJsonString(R"({
    "default_aoi_radius": 200,
    "default_aoi_hysteresis": 10,
    "max_aoi_radius": 1000
  })");
  ASSERT_TRUE(cfg.HasValue());
  ServerAppOptionBase::ApplyAll(*(*cfg)->Root());

  EXPECT_FLOAT_EQ(CellAppConfig::DefaultAoIRadius(), 200.f);
  EXPECT_FLOAT_EQ(CellAppConfig::DefaultAoIHysteresis(), 10.f);
  EXPECT_FLOAT_EQ(CellAppConfig::MaxAoIRadius(), 1000.f);
}

TEST_F(CellAppConfigTest, MissingKeysFallBackToDefaults) {
  // Only one key present — other two must return their registered defaults.
  auto cfg = DataSection::FromJsonString(R"({ "max_aoi_radius": 800 })");
  ASSERT_TRUE(cfg.HasValue());
  ServerAppOptionBase::ApplyAll(*(*cfg)->Root());

  EXPECT_FLOAT_EQ(CellAppConfig::DefaultAoIRadius(), 500.f);
  EXPECT_FLOAT_EQ(CellAppConfig::DefaultAoIHysteresis(), 5.f);
  EXPECT_FLOAT_EQ(CellAppConfig::MaxAoIRadius(), 800.f);
}

TEST_F(CellAppConfigTest, ReadWriteWatcherPathsAreRegistered) {
  WatcherRegistry reg;
  ServerAppOptionBase::RegisterAll(reg);

  // Paths must exist (exact string format of float serialization is the
  // watcher subsystem's business — we only assert presence here).
  EXPECT_TRUE(reg.Get("cellapp/default_aoi_radius").has_value());
  EXPECT_TRUE(reg.Get("cellapp/default_aoi_hysteresis").has_value());
  EXPECT_TRUE(reg.Get("cellapp/max_aoi_radius").has_value());
  EXPECT_TRUE(reg.Get("cellapp/witness_total_outbound_budget_bytes").has_value());
  EXPECT_TRUE(reg.Get("cellapp/witness_min_per_observer_budget_bytes").has_value());
  EXPECT_TRUE(reg.Get("cellapp/witness_max_per_observer_budget_bytes").has_value());
  EXPECT_TRUE(reg.Get("cellapp/witness_max_peers_per_tick").has_value());

  // Watchers are RW — ops can retune at runtime.
  EXPECT_TRUE(reg.Set("cellapp/default_aoi_radius", "250"));
  EXPECT_FLOAT_EQ(CellAppConfig::DefaultAoIRadius(), 250.f);
}
