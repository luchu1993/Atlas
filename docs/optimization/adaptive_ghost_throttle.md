# Adaptive Ghost Throttle

**Status:** 🔵 Deferred — `CellApp::TickGhostPump` is 0.045 % of
cellapp CPU under the current single-cellapp deployment. Not
exercised until multi-cellapp Spaces become routine.
**Subsystem:** `src/server/cellapp/ghost_maintainer.{h,cc}`

## What this would be

Ghost position updates currently fire at a fixed
`ghost_update_interval_ms` (50 ms) per Real entity per peer cellapp.
Two inefficiencies:

- **Static entities** waste bandwidth — 20 identical position
  payloads/sec/peer.
- **Fast movers** are under-sampled — at 20 m/s a 50 ms gap leaves
  the Ghost's AoI trigger position 1 m behind, delaying enter /
  leave events near boundaries.

The fix is a velocity-adaptive interval (e.g. 500 ms when
`speed_sq < 0.01`, 20 ms when `> 100`, linear between) plus a
skip-if-unchanged short-circuit before the interval check.

## Trigger to revisit

- A multi-cellapp Space topology in the production target shape, or
- `CellApp::TickGhostPump` exceeds ~3 % of cellapp CPU on a fresh
  capture.

## Caveats (when implementing)

- Velocity is currently derived from successive positions
  (`AvatarUpdate`); a derived value lags by one tick. For PvP
  combat-state transitions (entity starts attacking) the throttle
  needs a force-update path so the peer cellapp doesn't see stale
  ghost state across a state change.
- 500 ms intervals for stationary entities mean Ghost-side AoI
  enter events can lag by half a second. Acceptable for non-combat
  entities; not for anything where the cross-cell handoff is
  latency-sensitive.
