# Adaptive Ghost Throttle

**Priority:** P2
**Subsystem:** `src/server/cellapp/ghost_maintainer.cc`
**Impact:** Reduces cross-cell bandwidth for static entities, improves
responsiveness for fast-moving ones

## Current Behavior

Ghost position updates are throttled uniformly at 50ms intervals
(`ghost_update_interval_ms`). Every Real entity that has Ghost replicas on
peer CellApps sends a `GhostPositionUpdate` message at this fixed rate,
regardless of whether the entity moved.

## Problem

Two inefficiencies:

1. **Static entities waste bandwidth.** An entity standing still sends 20
   position updates per second to each peer CellApp — identical payloads
   every time.

2. **Fast-moving entities are under-sampled.** A player dashing at 20 m/s
   covers 1m between updates. On the peer CellApp, the Ghost's AoI trigger
   position lags by up to 1m, which can delay enter/leave events for
   observers near the boundary.

## Proposed Solution

Replace the fixed interval with a velocity-adaptive throttle:

```cpp
Duration GhostMaintainer::ComputeUpdateInterval(const CellEntity& entity) const {
  float speed_sq = LengthSq(entity.Velocity());
  if (speed_sq < 0.01f) {
    // Stationary: update every 500ms (2 Hz) — enough for AoI housekeeping
    return std::chrono::milliseconds(500);
  }
  if (speed_sq > 100.f) {  // > 10 m/s
    // Fast: update every 20ms (50 Hz) — keeps Ghost position tight
    return std::chrono::milliseconds(20);
  }
  // Linear interpolation between 20ms and 500ms
  float t = (speed_sq - 0.01f) / 99.99f;
  int ms = static_cast<int>(500.f - t * 480.f);
  return std::chrono::milliseconds(ms);
}
```

### Bandwidth Impact (100v100)

Assume 200 entities, 2 CellApps, all entities have Ghosts on the peer:

| | Fixed 50ms | Adaptive |
|---|-----------|----------|
| 100 stationary (buffing, dead) | 2000/s | 200/s |
| 50 walking (5 m/s) | 1000/s | 500/s |
| 50 sprinting (15 m/s) | 1000/s | 2500/s |
| **Total** | **4000/s** | **3200/s** (-20%) |

More importantly, the fast movers get 2.5x better Ghost fidelity, reducing
AoI boundary lag.

### Skip-If-Unchanged

Additionally, skip the update entirely if position hasn't changed since
the last broadcast:

```cpp
if (position == last_broadcast_position_ && !force_update) {
  return;  // nothing to send
}
```

This catches stationary entities even more cheaply than the interval check.

## Key Files

- `src/server/cellapp/ghost_maintainer.h` — Per-haunt adaptive interval
- `src/server/cellapp/ghost_maintainer.cc` — `ComputeUpdateInterval()`, skip-if-unchanged

## Risks

- Velocity must be available on the CellEntity. Currently position is set
  via `AvatarUpdate`; velocity may need to be derived from successive
  positions (finite difference).
- Very low update rates for stationary entities mean AoI enter events
  from a Ghost may be delayed by up to 500ms. Acceptable for non-combat
  entities; may need a "force update on state change" path for combat
  transitions (e.g. entity starts attacking).
