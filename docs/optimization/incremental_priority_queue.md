# Incremental Priority Queue

**Priority:** P1
**Subsystem:** `src/server/cellapp/witness.cc`
**Impact:** Witness per-tick CPU reduction of ~50% in dense AoI

> **Profiling note (100-client baseline, `b70b0ad`):** `Witness::Update::PriorityHeap`
> total = 74 ms over 120 s = **0.054% of tick CPU**. Not a bottleneck at 100
> peers per observer. This optimization becomes relevant at the 200-entity
> (100v100) target where heap cost scales to O(N log N) × M observers.

## Current Behavior

`Witness::Update()` rebuilds the entire priority heap every tick:

1. Clear the heap
2. For each peer in AoI, compute `distance_sq` to observer
3. Push into min-heap (closest first)

With 200 peers per observer and 100 observers, this is 20,000 distance
computations + heap insertions per tick.

## Problem

Most entities move slowly relative to tick rate. Between two consecutive ticks,
the relative ordering of 200 peers barely changes. Rebuilding the full heap
wastes ~90% of the work.

## Proposed Solution

Maintain a persistent priority queue per observer. Only update entries whose
distance changed significantly:

```cpp
void Witness::UpdatePriorities() {
  for (auto& entry : entity_cache_) {
    float new_dist_sq = DistanceSq(owner_pos_, entry.position);
    float delta = std::abs(new_dist_sq - entry.cached_dist_sq);
    if (delta > kDistSqThreshold) {
      entry.cached_dist_sq = new_dist_sq;
      // Sift up or down in the heap
      heap_.Update(entry.heap_index);
    }
  }
}
```

`kDistSqThreshold` can be tuned — e.g. 1.0 (1m displacement at 1m range,
10m displacement at 100m range).

### Heap Choice

Replace `std::push_heap` / `std::pop_heap` with an intrusive binary heap
where each `EntityCache` entry stores its heap index. This allows O(log N)
`Update()` on individual entries without rebuilding.

## Expected Improvement

| Scenario | Full Rebuild | Incremental |
|----------|-------------|-------------|
| 200 peers, 10% moved significantly | 200 inserts | 20 updates |
| Complexity per observer | O(N log N) | O(K log N), K << N |

## Key Files

- `src/server/cellapp/witness.h` — Add `cached_dist_sq`, `heap_index` to EntityCache
- `src/server/cellapp/witness.cc` — Replace full rebuild with incremental update

## Risks

- Stale priorities if threshold is too high. Entities may receive updates in
  suboptimal order. Acceptable because bandwidth budget is the real limiter.
- Intrusive heap adds memory overhead (one `size_t` per EntityCache entry).
