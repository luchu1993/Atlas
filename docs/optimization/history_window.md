# Replication History Window Expansion

**Priority:** P1
**Subsystem:** `src/server/cellapp/real_entity_data.h`
**Impact:** Reduces snapshot fallback frequency under load

## Current Behavior

Each entity keeps a sliding window of the last 8 replication frames in
`ReplicationState::history` (a bounded `std::deque`). When an observer falls
behind by more than 8 frames, the Witness falls back to a full snapshot
refresh instead of replaying deltas.

## Problem

At 30 Hz, 8 frames = 267ms of history. In a 100v100 scenario where bandwidth
budget limits each observer to ~40 entity updates per tick, an entity at the
tail of the priority queue may not be served for 5+ ticks (~167ms). Combined
with network jitter or a transient CPU spike, the 267ms window is easily
exceeded, triggering a snapshot.

Snapshots are expensive:
- Full entity state (~200-500 bytes vs. ~50-100 byte delta)
- Resets the observer's incremental view, causing visual "pop"
- Creates a bandwidth spike that pushes other entities into deficit

Under sustained load, snapshot triggers cascade: one snapshot consumes budget
that starves other entities, pushing *them* past the history window, creating
more snapshots.

## Proposed Solution

Expand the history window from 8 to 32 frames (~1067ms at 30 Hz). This
provides ~4x more tolerance for budget-induced delays.

```cpp
// real_entity_data.h
static constexpr std::size_t kReplicationHistoryWindow = 32;
```

### Memory Impact

Per entity: 32 frames x (~16 bytes header + ~80 bytes avg delta) = ~3 KB.
200 entities = ~600 KB. Acceptable.

### Configurable

Expose as a `CellAppConfig` setting so it can be tuned per deployment:

```cpp
uint32_t replication_history_window{32};
```

## Key Files

- `src/server/cellapp/real_entity_data.h` — Change `kReplicationHistoryWindow`
- `src/server/cellapp/cellapp_config.h` — Add config knob

## Risks

- Larger history means more memory per entity. At 1000 entities this is ~3 MB
  — still negligible.
- Observers that are genuinely disconnected will hold stale history longer
  before cleanup. Add a max-age TTL (e.g. 5 seconds) alongside the frame
  count limit.
