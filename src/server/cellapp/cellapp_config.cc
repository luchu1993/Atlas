#include "cellapp_config.h"

#include "server/server_app_option.h"

namespace atlas {

namespace {

// Watchers are ReadWrite so ops can retune live from the Watcher UI.
// Individual Witness instances capture their radius at construction time
// (or via an explicit SetAoIRadius call), so mutating these values at
// runtime affects only witnesses enabled *after* the change — matching
// BigWorld's bw.xml reload semantics.
ServerAppOption<float> s_default_aoi_radius{500.f, "default_aoi_radius",
                                            "cellapp/default_aoi_radius", WatcherMode::kReadWrite};

ServerAppOption<float> s_default_aoi_hysteresis{
    5.f, "default_aoi_hysteresis", "cellapp/default_aoi_hysteresis", WatcherMode::kReadWrite};

ServerAppOption<float> s_max_aoi_radius{500.f, "max_aoi_radius", "cellapp/max_aoi_radius",
                                        WatcherMode::kReadWrite};

ServerAppOption<float> s_load_smoothing_bias{
    0.05f, "load_smoothing_bias", "cellapp/load_smoothing_bias", WatcherMode::kReadWrite};

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

}  // namespace atlas
