# RangeList Grid Acceleration

**Priority:** P1
**Subsystem:** `src/lib/space/range_list.h`
**Impact:** Reduces AoI trigger cost from O(delta) to O(1) in dense regions

## Current Behavior

`RangeList` maintains two doubly-linked sorted lists (X-axis and Z-axis).
When an entity moves, its node is "shuffled" along the list to maintain sort
order. Each swap checks whether the two nodes should fire an AoI cross-
notification (enter/leave).

The cost per entity per tick is O(delta) where delta is the number of
positions the node passes during the shuffle. In sparse worlds, delta ~ 0-1.
In dense 100v100 battles with all entities in a 100m region, many entities
share similar coordinates, and bounded-speed movement causes frequent rank
swaps.

## Problem

With 200 entities shuffling in a 100m x 100m area at 30 Hz:
- Average displacement per tick: ~0.3m (10 m/s / 30 Hz)
- Entity spacing: ~7m (sqrt(10000/200))
- Expected swaps per entity per tick: ~0-1 (usually fine)

However, **worst case** with clustered spawns, teleports, or convergent
movement (e.g. two groups charging each other): delta spikes to O(N),
giving O(N^2) total shuffle cost for one tick. This manifests as frame
time spikes.

## Proposed Solution

Overlay a uniform grid on the RangeList. Each cell tracks which entities
reside in it. Movement updates the grid cell in O(1) and only triggers
cross-notifications for entities in the same or adjacent cells.

### Grid Design

```cpp
class SpatialGrid {
  float cell_size_;       // e.g. 50m — roughly AoI radius / 10
  int grid_width_;        // world_size / cell_size
  std::vector<std::vector<EntityNode*>> cells_;

  auto CellIndex(float x, float z) -> std::pair<int, int>;
  void Move(EntityNode* node, float old_x, float old_z, float new_x, float new_z);
};
```

### Integration

The grid supplements — not replaces — RangeList. RangeList continues to
handle the precise AoI enter/leave logic; the grid provides a spatial
pre-filter that skips shuffle steps for entities in distant cells.

```cpp
void RangeList::Shuffle(RangeNode* node) {
  // Before: walk the linked list checking every neighbor.
  // After:  only walk neighbors in the same or adjacent grid cells.
  auto [cx, cz] = grid_.CellIndex(node->x(), node->z());
  for (int dx = -1; dx <= 1; ++dx)
    for (int dz = -1; dz <= 1; ++dz)
      for (auto* neighbor : grid_.Cell(cx+dx, cz+dz))
        CheckCrossNotification(node, neighbor);
}
```

### Complexity

| Operation | Before | After |
|-----------|--------|-------|
| Move (sparse) | O(1) | O(1) |
| Move (dense, bounded speed) | O(delta) | O(K) where K = entities in 3x3 cells |
| Move (dense, teleport) | O(N) | O(K) |
| Worst case total | O(N^2) | O(N * K/N) = O(K) |

With cell_size = 50m and 200 entities in 100m: K ~ 50 per 3x3 region,
bounded regardless of teleport distance.

## Key Files

- `src/lib/space/range_list.h` — Add grid overlay
- `src/lib/space/range_list.cc` — Grid-accelerated shuffle
- `src/lib/space/spatial_grid.h` — New file, grid data structure

## Risks

- Grid cell size must be tuned. Too small: high memory, too many empty cells.
  Too large: no acceleration benefit. 50m is a reasonable starting point.
- Extra memory: 200x200 grid for a 10km world = 40,000 cells x 8 bytes = 320 KB.
- Grid must handle entities outside the grid bounds (clamp to edge cells).
