# Distance-Based LOD for Witness Replication

**Priority:** P0
**Subsystem:** `src/server/cellapp/witness.cc`
**Impact:** Broadcast volume reduction of 60-80% in dense scenarios

## Current Behavior

Every entity within the AoI radius (default 500m) is replicated at the same
frequency — once per tick (~30 Hz). In a 100v100 battle where all 200 entities
are within range, each observer receives ~200 updates per tick regardless of
distance.

## Problem

The O(N) per-observer cost becomes O(N*M) total where M = observer count. With
N=200, M=100, this is 20,000 property syncs per tick. The fixed 4KB/tick
bandwidth budget can only process ~30-40 entities, starving the rest into
deficit carry-forward. This is the primary cause of high Echo RTT (p99 > 5s)
seen in stress tests.

## Proposed Solution

Partition the AoI into distance bands, each with a different update frequency:

| Band | Range | Update Rate | Ratio |
|------|-------|-------------|-------|
| Close | 0 - 50m | Every tick (30 Hz) | 1x |
| Medium | 50 - 200m | Every 3rd tick (10 Hz) | 3x reduction |
| Far | 200 - 500m | Every 6th tick (5 Hz) | 6x reduction |

### Algorithm

In `Witness::Update()`, after rebuilding the priority heap:

```cpp
for (auto& peer : priority_queue_) {
  float dist_sq = peer.distance_sq;
  int interval = (dist_sq < kCloseSq) ? 1
               : (dist_sq < kMediumSq) ? 3
               : 6;
  if (tick_count_ % interval != peer.phase_offset) continue;
  // ... existing send logic
}
```

`phase_offset` is set at AoI-enter time (`entity_id % interval`) to spread
updates across ticks and avoid burst patterns.

### Bandwidth Impact (100v100)

| | Before | After |
|---|--------|-------|
| Entities in Close | ~20 | 20 × 30 Hz = 600/s |
| Entities in Medium | ~80 | 80 × 10 Hz = 800/s |
| Entities in Far | ~100 | 100 × 5 Hz = 500/s |
| **Total updates/s** | **6000/s** | **1900/s** (-68%) |

## Configuration

```cpp
// cellapp_config.h
float lod_close_radius{50.f};
float lod_medium_radius{200.f};
int lod_close_interval{1};
int lod_medium_interval{3};
int lod_far_interval{6};
```

## Key Files

- `src/server/cellapp/witness.h` — Add LOD band config, phase offset to EntityCache
- `src/server/cellapp/witness.cc` — Modify Update() loop with interval check
- `src/server/cellapp/cellapp_config.h` — Add LOD radius/interval settings

## Risks

- Distant entities may appear to "teleport" when their infrequent updates
  arrive. Client-side interpolation/extrapolation must smooth this.
- Phase offset prevents all far entities from updating on the same tick, but
  uneven band populations could still cause periodic spikes.
