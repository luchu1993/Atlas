# Distance-Based LOD for Witness Replication

**Status:** ✅ Shipped.
**Subsystem:** `src/server/cellapp/witness.cc`, `src/server/cellapp/cellapp_config.h`

## Design

AoI peers are partitioned into three distance bands; each band updates at a
different cadence so far peers don't pay the full per-tick replication cost.

| Band | Range (squared check) | Update cadence |
|---|---|---|
| Close | `d² < LodConfig::dist_sq[0]` (≈ 25 m) | every tick |
| Medium | `d² < LodConfig::dist_sq[1]` (≈ 100 m) | every 3rd tick |
| Far | `d² ≥ LodConfig::dist_sq[1]` | every 6th tick |

`Witness::Update` snapshots `LodConfig`, calls `LodConfig::BandIndex`
for each peer, then pumps each band at its configured interval. Each
`EntityCache` entry is assigned a `lod_enter_phase` at AoI-enter time,
so the first window of a peer is naturally staggered without baking
`entity_id % interval` into the schedule — prevents bursty patterns
where many far peers tick on the same frame.

## Knobs

```cpp
// cellapp_config.h
float    witness_lod_close_distance_m;        // 25
float    witness_lod_medium_distance_m;       // 100
uint32_t witness_lod_close_interval_ticks;    // 1
uint32_t witness_lod_medium_interval_ticks;   // 3
uint32_t witness_lod_far_interval_ticks;      // 6
uint32_t witness_lod_close_max_peers_per_tick;   // 64
uint32_t witness_lod_medium_max_peers_per_tick;  // 64
uint32_t witness_lod_far_max_peers_per_tick;     // 64
uint32_t witness_lod_starvation_threshold_ticks; // 30
```

## Caveats

- Distant entities can appear to teleport on infrequent updates; client
  interpolation/extrapolation is responsible for visual smoothing.
- Phase offset spreads load but uneven band populations can still produce
  small periodic spikes — tolerable at current scale, revisit only if a
  capture shows it driving tail latency.
