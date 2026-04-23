#include "cellapp_config.h"

#include "server/server_app_option.h"

namespace atlas {

namespace {

// Watchers are ReadWrite so ops can retune live from the Watcher UI.
// Individual Witness instances capture their radius at construction time
// (or via an explicit SetAoIRadius call), so mutating these values at
// runtime affects only witnesses enabled *after* the change.
ServerAppOption<float> s_default_aoi_radius{500.f, "default_aoi_radius",
                                            "cellapp/default_aoi_radius", WatcherMode::kReadWrite};

ServerAppOption<float> s_default_aoi_hysteresis{
    5.f, "default_aoi_hysteresis", "cellapp/default_aoi_hysteresis", WatcherMode::kReadWrite};

ServerAppOption<float> s_max_aoi_radius{500.f, "max_aoi_radius", "cellapp/max_aoi_radius",
                                        WatcherMode::kReadWrite};

ServerAppOption<float> s_load_smoothing_bias{
    0.05f, "load_smoothing_bias", "cellapp/load_smoothing_bias", WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_ghost_update_interval_ms{
    50u, "ghost_update_interval_ms", "cellapp/ghost_update_interval_ms", WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_per_observer_budget_bytes{
    4096u, "witness_per_observer_budget_bytes", "cellapp/witness_per_observer_budget_bytes",
    WatcherMode::kReadWrite};

}  // namespace

auto CellAppConfig::DefaultAoIRadius() -> float {
  return s_default_aoi_radius.Value();
}

auto CellAppConfig::DefaultAoIHysteresis() -> float {
  return s_default_aoi_hysteresis.Value();
}

auto CellAppConfig::MaxAoIRadius() -> float {
  return s_max_aoi_radius.Value();
}

auto CellAppConfig::LoadSmoothingBias() -> float {
  return s_load_smoothing_bias.Value();
}

auto CellAppConfig::GhostUpdateIntervalMs() -> uint32_t {
  return s_ghost_update_interval_ms.Value();
}

auto CellAppConfig::WitnessPerObserverBudgetBytes() -> uint32_t {
  return s_witness_per_observer_budget_bytes.Value();
}

}  // namespace atlas
