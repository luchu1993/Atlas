# RangeList Grid Acceleration

**Status:** 🔵 Deferred — `Space::Tick` is 0.019 % of cellapp CPU at
200 cli. Not on any visible critical path.
**Subsystem:** `src/lib/space/range_list.h`

## What this would be

`RangeList` maintains two doubly-linked sorted lists (X and Z axes)
and shuffles entity nodes along each list as positions update. Each
swap checks for AoI cross-notifications. Cost per tick is
`O(δ)` per entity, where δ is the number of rank positions the node
crossed.

Sparse worlds: δ ≈ 0–1, no problem. Worst case (clustered spawns,
teleports, two groups converging) δ spikes to `O(N)` and the total
shuffle becomes `O(N²)`.

A uniform grid overlay would cap the cross-notification scan at a
fixed `K` neighbours in the same / adjacent cells, regardless of
displacement magnitude. The grid supplements RangeList rather than
replacing it — RangeList still owns precise enter / leave logic.

## Trigger to revisit

- `Space::Tick` exceeds ~1 % of cellapp tick on a fresh capture, or
- A reproducible scenario (mass teleport, charge-collide PvP) puts
  visible jank on the move path.

## Cost when implementing

Memory: a 200 × 200 grid for a 10 km world at 50 m cells is ~320 KB.
Cell size needs scenario tuning — too small wastes memory on empty
cells, too large reverts to `O(N)` neighbours.
