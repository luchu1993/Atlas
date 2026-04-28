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

// Demand-based allocator: NIC-shaped cellapp-wide cap.  At the default
// 15 Hz cellapp cadence the 4 MB/tick cap is 60 MB/s — leaves ~50%
// headroom under a 1 GbE link (125 MB/s) for framing, retransmit, and
// other server traffic.  Sized off the 500-client baseline (66a278c
// 2026-04-28): 500 observers × ~7.3 KB/tick steady-state ≈ 3.65 MB/tick
// fleet demand, which exceeded the prior 1.6 MB cap by 2.3× and pushed
// every observer into deficit (103 `bandwidth deficit` warnings in the
// 137 s capture).  Lower this on hardware with smaller NICs or when
// the cellapp shares a NIC with other heavy services.
ServerAppOption<uint32_t> s_witness_total_outbound_cap_bytes{
    4194304u, "witness_total_outbound_cap_bytes", "cellapp/witness_total_outbound_cap_bytes",
    WatcherMode::kReadWrite};

// Per-peer demand multiplier.  200 B/tick mirrors the per-peer share of
// observed outbound at 500 observers / 150 m AoI / 200 m walk-range:
// 7.3 KB/tick / ~36 in-AoI peers ≈ 200 B/peer (≈ 30 B position +
// 50 B property delta + 120 B amortised enter snapshot under churn).
// The 200-client baseline measured 150 B/peer; the bump to 200 reflects
// increased AoI density at higher client counts and produces a more
// accurate `demand` estimate for the proportional allocator.  Sparse
// scenes still get a comfortable margin via
// `min_per_observer_budget_bytes`. Enter spikes that exceed the steady
// estimate carry over via bandwidth_deficit_, costing one tick
// (~66 ms at 15 Hz) of allocation lag.
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
