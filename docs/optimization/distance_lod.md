# Distance-Based LOD for Witness Replication

**Status:** ✅ Shipped (`54bd9df`).
**Subsystem:** `src/server/cellapp/witness.cc`, `src/server/cellapp/cellapp_config.h`

## Design

AoI peers are partitioned into three distance bands; each band updates at a
different cadence so far peers don't pay the full per-tick replication cost.

| Band | Range (squared check) | Update cadence |
|---|---|---|
| Close | `d² < kLodCloseSq` (≈ 25 m) | every tick |
| Medium | `d² < kLodMediumSq` (≈ 100 m) | every 3rd tick |
| Far | `d² ≥ kLodMediumSq` | every 6th tick |

The cadence check lives in `Witness::LodIntervalForDistSq`. Each
`EntityCache` entry is assigned a `lod_enter_phase` at AoI-enter time, so
the first window of a peer is naturally staggered without baking
`entity_id % interval` into the schedule — prevents bursty patterns where
many far peers tick on the same frame.

## Knobs

```cpp
// cellapp_config.h
float lod_close_radius;
float lod_medium_radius;
int   lod_close_interval;   // 1
int   lod_medium_interval;  // 3
int   lod_far_interval;     // 6
```

## Caveats

- Distant entities can appear to teleport on infrequent updates; client
  interpolation/extrapolation is responsible for visual smoothing.
- Phase offset spreads load but uneven band populations can still produce
  small periodic spikes — tolerable at current scale, revisit only if a
  capture shows it driving tail latency.
