# Incremental Priority Queue

**Status:** 🔵 Deferred — `Witness::Update::PriorityHeap` is 0.40 % of
CPU at 200 cli (and dropped further after the witness channel cache
landed). Not on any visible critical path.
**Subsystem:** `src/server/cellapp/witness.cc`

## What this would be

`Witness::Update()` currently rebuilds the per-observer priority
heap each tick (clear → recompute `distance_sq` for every AoI peer
→ heap-push). At 200 peers × 100 observers that's 20 k distance
computations + heap inserts per tick.

Most peers move slowly relative to tick rate, so the rank order
barely changes between ticks. An intrusive heap that stores its
index on each `EntityCache` entry would let us update only entries
whose squared distance moved past a threshold — `O(K log N)` instead
of `O(N log N)`.

## Trigger to revisit

Open this doc when a fresh capture shows
`Witness::Update::PriorityHeap` exceeding ~5 % of cellapp CPU, or
when the per-observer peer count crosses 500 on the dense PvP path.
Until then, the rebuild cost is below other zones worth attacking.
