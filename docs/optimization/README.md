# Optimization Roadmap

This directory documents planned optimizations for Atlas Engine, organized by
subsystem. Each document describes the current bottleneck, proposed solution,
expected impact, and implementation notes.

## Target Scenario

**100v100 large-scale battle**: 200 entities in a single Space, all within AoI
range, with complex combat logic running in C# scripts. This is the design
ceiling Atlas must support comfortably.

## Documents

| Document | Subsystem | Priority |
|----------|-----------|----------|
| [distance_lod.md](distance_lod.md) | Witness replication | P0 |
| [property_dirty_flags.md](property_dirty_flags.md) | Entity replication | P0 |
| [adaptive_bandwidth.md](adaptive_bandwidth.md) | Witness bandwidth | P0 |
| [incremental_priority_queue.md](incremental_priority_queue.md) | Witness update | P1 |
| [history_window.md](history_window.md) | Replication catch-up | P1 |
| [rangelist_grid.md](rangelist_grid.md) | Spatial partitioning | P1 |
| [group_broadcast.md](group_broadcast.md) | Network fan-out | P2 |
| [visibility_culling.md](visibility_culling.md) | AoI filtering | P2 |
| [adaptive_ghost_throttle.md](adaptive_ghost_throttle.md) | Cross-cell sync | P2 |

## Priority Definitions

- **P0** — Required to make 100v100 functional (solves "can it run")
- **P1** — Required to make 100v100 smooth (solves "does it run well")
- **P2** — Polish and advanced features (solves "does it feel good")
