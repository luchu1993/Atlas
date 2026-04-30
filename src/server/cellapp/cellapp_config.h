#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_

#include <cstdint>

namespace atlas {

// Process-scoped CellApp configuration accessors backed by ServerAppOption
// statics. Reloads follow the ServerAppOption watcher semantics; most
// knobs bind at entity / witness construction, so runtime edits affect
// only instances created afterwards.
class CellAppConfig {
 public:
  // Initial AoI radius (metres) assigned to newly enabled Witnesses.
  // JSON key: `default_aoi_radius`. Default 500 m. Runtime overrides go
  // through Witness::SetAoIRadius, not through mutation of this value.
  [[nodiscard]] static auto DefaultAoIRadius() -> float;

  // Initial hysteresis band (metres) around the AoI boundary. Leave
  // events fire at `radius + hysteresis`; enter events fire at `radius`.
  // JSON key: `default_aoi_hysteresis`. Default 5.0 m.
  [[nodiscard]] static auto DefaultAoIHysteresis() -> float;

  // Upper clamp applied inside Witness::SetAoIRadius. Also applied on
  // ctor when a custom radius is supplied. JSON key: `max_aoi_radius`.
  // Default 500 m.
  [[nodiscard]] static auto MaxAoIRadius() -> float;

  // EWMA smoothing bias used by CellApp::UpdatePersistentLoad. The
  // load estimate is `(1-bias)*prev + bias * normalised_work_time`.
  // Small bias = heavy smoothing, slow reaction to spikes but stable;
  // large bias = responsive but noisy. JSON key: `load_smoothing_bias`.
  // Default 0.05.
  [[nodiscard]] static auto LoadSmoothingBias() -> float;

  // Minimum interval (milliseconds) between GhostPositionUpdate /
  // GhostDelta broadcasts a single Real can emit to a given Haunt.
  // Caps wire cost for fast-moving entities: position updates
  // coalesce into one send per interval instead of one per tick.
  // JSON key: `ghost_update_interval_ms`. Default 50 ms (~20 Hz),
  // i.e. roughly every other tick at a 30 Hz cadence. Set to 0 to
  // disable and broadcast every tick.
  [[nodiscard]] static auto GhostUpdateIntervalMs() -> uint32_t;

  // Cellapp-wide ceiling on per-tick witness outbound bytes. The demand-
  // based allocator scales every observer's request down proportionally
  // when the sum of requests exceeds this cap.  Sized to the host's NIC
  // budget at the configured tick rate (default 4 MB/tick = 60 MB/s
  // at the default 15 Hz cellapp cadence - ~ 50 % of a 1 GbE NIC).
  // JSON key: `witness_total_outbound_cap_bytes`. Default 4194304 (4 MB).
  [[nodiscard]] static auto WitnessTotalOutboundCapBytes() -> uint32_t;

  // Per-peer demand multiplier the allocator uses to estimate an
  // observer's outbound bytes for the upcoming tick:
  //   demand = peers_in_aoi * WitnessPerPeerBytes() + last_tick_deficit
  // Set to roughly the average per-peer outbound size in your scene
  // (steady-state position/property delta + amortised enter snapshot).
  // JSON key: `witness_per_peer_bytes`. Default 200.
  [[nodiscard]] static auto WitnessPerPeerBytes() -> uint32_t;

  // Floor on the per-observer allocation computed from the total budget.
  // Prevents starvation when observer_count is large but budget is tight.
  // JSON key: `witness_min_per_observer_budget_bytes`. Default 1024 (1 KB).
  [[nodiscard]] static auto WitnessMinPerObserverBudgetBytes() -> uint32_t;

  // Ceiling on the per-observer allocation. Lets sparse spaces send more
  // per observer without unboundedly inflating individual send windows.
  // JSON key: `witness_max_per_observer_budget_bytes`. Default 16384 (16 KB).
  [[nodiscard]] static auto WitnessMaxPerObserverBudgetBytes() -> uint32_t;

  // Hard cap on SendEntityUpdate calls per observer per tick. Bounds
  // serialisation CPU even when the byte budget would allow more - peers
  // not served this tick stay in the priority queue and surface at the
  // top next tick, so the nearest peers are always served first. Tune
  // upward if dense scenes show stale peers; downward if Witness::Update
  // dominates the tick.
  // JSON key: `witness_max_peers_per_tick`. Default 64.
  [[nodiscard]] static auto WitnessMaxPeersPerTick() -> uint32_t;

  // Caps pump eligibility without changing AoI membership.
  // JSON key: `witness_max_aoi_peers`. Default 50.
  [[nodiscard]] static auto WitnessMaxAoIPeers() -> uint32_t;

  // Skipped peers are promoted above all real distances after this many ticks; 0 disables.
  // JSON key: `witness_starvation_threshold_ticks`. Default 30.
  [[nodiscard]] static auto WitnessStarvationThresholdTicks() -> uint32_t;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_
