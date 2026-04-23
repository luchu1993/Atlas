#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_

#include <cstdint>

namespace atlas {

// Process-scoped CellApp configuration accessors backed by ServerAppOption
// statics. BigWorld parity: mirrors `CellAppConfig` in
// bigworld/server/cellapp/cellapp_config.hpp, where the same knobs live
// under `<cellApp>...</cellApp>` in bw.xml.
class CellAppConfig {
 public:
  // Initial AoI radius (metres) assigned to newly enabled Witnesses.
  // JSON key: `default_aoi_radius`. Default 500 m (BigWorld parity).
  // Runtime overrides go through Witness::SetAoIRadius, not through
  // mutation of this value.
  [[nodiscard]] static auto DefaultAoIRadius() -> float;

  // Initial hysteresis band (metres) around the AoI boundary. Leave
  // events fire at `radius + hysteresis`; enter events fire at `radius`.
  // JSON key: `default_aoi_hysteresis`. Default 5.0 m
  // (matches BigWorld `Witness::aoiHyst_` init value in witness.cpp:136).
  [[nodiscard]] static auto DefaultAoIHysteresis() -> float;

  // Upper clamp applied inside Witness::SetAoIRadius. Also applied on
  // ctor when a custom radius is supplied. JSON key: `max_aoi_radius`.
  // Default 500 m (BigWorld parity with `CellAppConfig::maxAoIRadius`).
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
  // JSON key: `ghost_update_interval_ms`. Default 50 ms (≈20 Hz),
  // i.e. roughly every other tick at a 30 Hz cadence. Set to 0 to
  // disable and broadcast every tick.
  [[nodiscard]] static auto GhostUpdateIntervalMs() -> uint32_t;
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_
