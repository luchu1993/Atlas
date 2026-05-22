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
  // JSON key: `witness_max_per_observer_budget_bytes`. Default 65536 (64 KB).
  [[nodiscard]] static auto WitnessMaxPerObserverBudgetBytes() -> uint32_t;

  // LOD distance thresholds in metres. Peer distance < Close → Close band;
  // < Medium → Medium band; otherwise Far band. JSON keys:
  // `witness_lod_close_distance_m`, `witness_lod_medium_distance_m`.
  // Defaults 25 m / 100 m.
  [[nodiscard]] static auto WitnessLodCloseDistanceM() -> float;
  [[nodiscard]] static auto WitnessLodMediumDistanceM() -> float;

  // Update interval (ticks) for each LOD band. A peer in band B becomes
  // eligible every interval(B) ticks. JSON keys:
  // `witness_lod_close_interval_ticks`, ... Defaults 1 / 3 / 6.
  [[nodiscard]] static auto WitnessLodCloseIntervalTicks() -> uint32_t;
  [[nodiscard]] static auto WitnessLodMediumIntervalTicks() -> uint32_t;
  [[nodiscard]] static auto WitnessLodFarIntervalTicks() -> uint32_t;

  // Per-band hard cap on SendEntityUpdate calls per observer per tick.
  // Bands no longer compete for a shared budget — far peers can't starve
  // close ones and vice versa. JSON keys:
  // `witness_lod_close_max_peers_per_tick`, ... Defaults 64 each.
  [[nodiscard]] static auto WitnessLodCloseMaxPeersPerTick() -> uint32_t;
  [[nodiscard]] static auto WitnessLodMediumMaxPeersPerTick() -> uint32_t;
  [[nodiscard]] static auto WitnessLodFarMaxPeersPerTick() -> uint32_t;

  // Within a band, peers past this many ticks since the last service are
  // promoted above the closest-N rank cut so the band always drains. 0
  // disables. JSON key: `witness_lod_starvation_threshold_ticks`. Default 30.
  [[nodiscard]] static auto WitnessLodStarvationThresholdTicks() -> uint32_t;

  // Per-tick byte budget for AoI Enter envelopes on a single Witness; excess
  // peers are deferred to the next tick. Caps the initial-burst load on the
  // reliable-UDP send window when a fresh observer joins a dense scene.
  // JSON key: `witness_enter_bytes_per_tick`. Default 4096 (4 KB).
  [[nodiscard]] static auto WitnessEnterBytesPerTick() -> uint32_t;

  // Hard cap on per-Witness Enter envelopes per tick. Backstop in case
  // average Enter size is small but count would still flood the channel.
  // Default 4 spreads ~200 peers across ~50 ticks (=2.5s at 20Hz) so the
  // client sees a smooth fade-in rather than a single batched frame.
  // JSON key: `witness_max_enters_per_tick`. Default 4.
  [[nodiscard]] static auto WitnessMaxEntersPerTick() -> uint32_t;

  // Ghost-band half-width around every BSP split; suppresses boundary
  // thrashing. JSON key `bsp_ghost_region_metres`, default 15 m.
  [[nodiscard]] static auto BspGhostRegionMetres() -> float;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_
