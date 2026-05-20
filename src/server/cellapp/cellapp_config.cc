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
    65536u, "witness_max_per_observer_budget_bytes",
    "cellapp/witness_max_per_observer_budget_bytes", WatcherMode::kReadWrite};

ServerAppOption<float> s_witness_lod_close_distance_m{
    25.f, "witness_lod_close_distance_m", "cellapp/witness_lod_close_distance_m",
    WatcherMode::kReadWrite};

ServerAppOption<float> s_witness_lod_medium_distance_m{
    100.f, "witness_lod_medium_distance_m", "cellapp/witness_lod_medium_distance_m",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_lod_close_interval_ticks{
    1u, "witness_lod_close_interval_ticks", "cellapp/witness_lod_close_interval_ticks",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_lod_medium_interval_ticks{
    3u, "witness_lod_medium_interval_ticks", "cellapp/witness_lod_medium_interval_ticks",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_lod_far_interval_ticks{
    6u, "witness_lod_far_interval_ticks", "cellapp/witness_lod_far_interval_ticks",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_lod_close_max_peers_per_tick{
    64u, "witness_lod_close_max_peers_per_tick", "cellapp/witness_lod_close_max_peers_per_tick",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_lod_medium_max_peers_per_tick{
    64u, "witness_lod_medium_max_peers_per_tick", "cellapp/witness_lod_medium_max_peers_per_tick",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_lod_far_max_peers_per_tick{
    64u, "witness_lod_far_max_peers_per_tick", "cellapp/witness_lod_far_max_peers_per_tick",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_lod_starvation_threshold_ticks{
    30u, "witness_lod_starvation_threshold_ticks",
    "cellapp/witness_lod_starvation_threshold_ticks", WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_enter_bytes_per_tick{
    4096u, "witness_enter_bytes_per_tick", "cellapp/witness_enter_bytes_per_tick",
    WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_witness_max_enters_per_tick{
    4u, "witness_max_enters_per_tick", "cellapp/witness_max_enters_per_tick",
    WatcherMode::kReadWrite};

ServerAppOption<float> s_bsp_ghost_region_metres{
    1.0f, "bsp_ghost_region_metres", "cellapp/bsp_ghost_region_metres", WatcherMode::kReadWrite};

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

auto CellAppConfig::WitnessLodCloseDistanceM() -> float {
  return s_witness_lod_close_distance_m.Value();
}

auto CellAppConfig::WitnessLodMediumDistanceM() -> float {
  return s_witness_lod_medium_distance_m.Value();
}

auto CellAppConfig::WitnessLodCloseIntervalTicks() -> uint32_t {
  return s_witness_lod_close_interval_ticks.Value();
}

auto CellAppConfig::WitnessLodMediumIntervalTicks() -> uint32_t {
  return s_witness_lod_medium_interval_ticks.Value();
}

auto CellAppConfig::WitnessLodFarIntervalTicks() -> uint32_t {
  return s_witness_lod_far_interval_ticks.Value();
}

auto CellAppConfig::WitnessLodCloseMaxPeersPerTick() -> uint32_t {
  return s_witness_lod_close_max_peers_per_tick.Value();
}

auto CellAppConfig::WitnessLodMediumMaxPeersPerTick() -> uint32_t {
  return s_witness_lod_medium_max_peers_per_tick.Value();
}

auto CellAppConfig::WitnessLodFarMaxPeersPerTick() -> uint32_t {
  return s_witness_lod_far_max_peers_per_tick.Value();
}

auto CellAppConfig::WitnessLodStarvationThresholdTicks() -> uint32_t {
  return s_witness_lod_starvation_threshold_ticks.Value();
}

auto CellAppConfig::WitnessEnterBytesPerTick() -> uint32_t {
  return s_witness_enter_bytes_per_tick.Value();
}

auto CellAppConfig::WitnessMaxEntersPerTick() -> uint32_t {
  return s_witness_max_enters_per_tick.Value();
}

auto CellAppConfig::BspGhostRegionMetres() -> float {
  return s_bsp_ghost_region_metres.Value();
}

}  // namespace atlas
