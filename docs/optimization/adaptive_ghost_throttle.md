# Adaptive Ghost Throttle

**Status:** 🔵 Deferred — the last retained profiling baseline showed
`CellApp::TickGhostPump` at 0.045 % of cellapp CPU. Phase 11 now has
multi-cellapp Real / Ghost coverage, but this path has not crossed an
optimization trigger in the recorded stress and Tracy baselines.
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

- `CellApp::TickGhostPump` exceeds ~3 % of cellapp CPU on a fresh
  multi-cellapp capture, or
- Cross-cell combat or migration tests show stale Ghost state due to
  the fixed interval.

## Caveats (when implementing)

- No production velocity source currently feeds `TickGhostPump`.
  `RealEntityData::UpdateVelocity` exists but is not wired into the
  pump; an implementation must choose the authority source first
  (Phase 14 `MovementStateStore` or a local position-sample fallback).
  PvP combat-state transitions still need a force-update path so the
  peer cellapp doesn't see stale ghost state across a state change.
- 500 ms intervals for stationary entities mean Ghost-side AoI
  enter events can lag by half a second. Acceptable for non-combat
  entities; not for anything where the cross-cell handoff is
  latency-sensitive.
