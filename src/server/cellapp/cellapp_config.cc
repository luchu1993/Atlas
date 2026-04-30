#include "cellapp_config.h"

#include "server/server_app_option.h"

namespace atlas {

namespace {

// Individual Witness instances capture their radius at construction time
// (or via an explicit SetAoIRadius call), so mutating these values at
// runtime affects only newly enabled witnesses.
ServerAppOption<float> s_default_aoi_radius{150.f, "default_aoi_radius",
                                            "cellapp/default_aoi_radius", WatcherMode::kReadWrite};

ServerAppOption<float> s_default_aoi_hysteresis{
    5.f, "default_aoi_hysteresis", "cellapp/default_aoi_hysteresis", WatcherMode::kReadWrite};

ServerAppOption<float> s_max_aoi_radius{500.f, "max_aoi_radius", "cellapp/max_aoi_radius",
                                        WatcherMode::kReadWrite};

ServerAppOption<float> s_load_smoothing_bias{
    0.05f, "load_smoothing_bias", "cellapp/load_smoothing_bias", WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_ghost_update_interval_ms{
    50u, "ghost_update_interval_ms", "cellapp/ghost_update_interval_ms", WatcherMode::kReadWrite};

// NIC-shaped cellapp-wide cap; lower it on smaller shared links.
ServerAppOption<uint32_t> s_witness_total_outbound_cap_bytes{
    4194304u, "witness_total_outbound_cap_bytes", "cellapp/witness_total_outbound_cap_bytes",
    WatcherMode::kReadWrite};

// Per-peer demand estimate for proportional witness budget allocation.
ServerAppOption<uint32_t> s_witness_per_peer_bytes{
    200u, "witness_per_peer_bytes", "cellapp/witness_per_peer_bytes", WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_min_per_observer_budget_bytes{
    1024u, "witness_min_per_observer_budget_bytes", "cellapp/witness_min_per_observer_budget_bytes",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_max_per_observer_budget_bytes{
    16384u, "witness_max_per_observer_budget_bytes",
    "cellapp/witness_max_per_observer_budget_bytes", WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_max_peers_per_tick{64u, "witness_max_peers_per_tick",
                                                       "cellapp/witness_max_peers_per_tick",
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

auto CellAppConfig::WitnessTotalOutboundCapBytes() -> uint32_t {
  return s_witness_total_outbound_cap_bytes.Value();
}

auto CellAppConfig::WitnessPerPeerBytes() -> uint32_t {
  return s_witness_per_peer_bytes.Value();
}

auto CellAppConfig::WitnessMinPerObserverBudgetBytes() -> uint32_t {
  return s_witness_min_per_observer_budget_bytes.Value();
}

auto CellAppConfig::WitnessMaxPerObserverBudgetBytes() -> uint32_t {
  return s_witness_max_per_observer_budget_bytes.Value();
}

auto CellAppConfig::WitnessMaxPeersPerTick() -> uint32_t {
  return s_witness_max_peers_per_tick.Value();
}

}  // namespace atlas
