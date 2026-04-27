#include "cellapp_config.h"

#include "server/server_app_option.h"

namespace atlas {

namespace {

// Watchers are ReadWrite so ops can retune live from the Watcher UI.
// Individual Witness instances capture their radius at construction time
// (or via an explicit SetAoIRadius call), so mutating these values at
// runtime affects only witnesses enabled *after* the change.
//
// Default radius matches the BDO/Dragon Nest viewport scale (~80–150 m)
// — a player sees roughly the screen frustum, not "the entire shard".
// 150 m fits typical action-MMO sight line; siege/raid scenes that
// genuinely need more push their witnesses up via SetAoIRadius (capped
// by max_aoi_radius below). Larger defaults (historical 500 m) made
// N×M Witness::Update cost dominate cellapp tick once entity density
// crossed ~50 visible peers per observer.
ServerAppOption<float> s_default_aoi_radius{150.f, "default_aoi_radius",
                                            "cellapp/default_aoi_radius", WatcherMode::kReadWrite};

ServerAppOption<float> s_default_aoi_hysteresis{
    5.f, "default_aoi_hysteresis", "cellapp/default_aoi_hysteresis", WatcherMode::kReadWrite};

// max_aoi_radius is the upper bound that SetAoIRadius will accept; keep it
// at the historical 500 m so siege/raid scenes can still opt in via script.
ServerAppOption<float> s_max_aoi_radius{500.f, "max_aoi_radius", "cellapp/max_aoi_radius",
                                        WatcherMode::kReadWrite};

ServerAppOption<float> s_load_smoothing_bias{
    0.05f, "load_smoothing_bias", "cellapp/load_smoothing_bias", WatcherMode::kReadWrite};

ServerAppOption<uint32_t> s_ghost_update_interval_ms{
    50u, "ghost_update_interval_ms", "cellapp/ghost_update_interval_ms", WatcherMode::kReadWrite};

// Per-tick cellapp-wide outbound replication budget. Divided across all
// observers in TickWitnesses, then clamped per-observer to [min, max].
// 800 KB/tick @ 10 Hz = 8 MB/s; with 200 observers each gets 4 KB/tick
// (≈ 40 KB/s) — within real-world MMORPG per-client downstream
// (BDO / Dragon Nest see 30–80 KB/s in dense scenes). The previous
// 400 KB/tick clamped each observer to 2 KB at the 200-client mark,
// which left witness priority queues non-empty every tick.
ServerAppOption<uint32_t> s_witness_total_outbound_budget_bytes{
    819200u, "witness_total_outbound_budget_bytes", "cellapp/witness_total_outbound_budget_bytes",
    WatcherMode::kReadWrite};

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

auto CellAppConfig::WitnessTotalOutboundBudgetBytes() -> uint32_t {
  return s_witness_total_outbound_budget_bytes.Value();
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
