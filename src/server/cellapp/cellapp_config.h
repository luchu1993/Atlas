#ifndef ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_
#define ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_

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
};

}  // namespace atlas

#endif  // ATLAS_SERVER_CELLAPP_CELLAPP_CONFIG_H_
