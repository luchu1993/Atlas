# Optimization Roadmap

This directory documents planned optimizations for Atlas Engine, organized by
subsystem. Each document describes the current bottleneck, proposed solution,
expected impact, and implementation notes.

## Target Scenario

**100v100 large-scale battle**: 200 entities in a single Space, all within AoI
range, with complex combat logic running in C# scripts. This is the design
ceiling Atlas must support comfortably.

## Profiling Baseline

Measured with `run_baseline_profile.sh` (100 clients, 120s, profile,
`b70b0ad`). Tracy captures in `.tmp/prof/baseline/`.

### CellApp (10 Hz, budget = 100 ms/tick)

| Zone | mean | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| Tick | 13.1 ms | 10.9 ms | 24.1 ms | 47.6 ms | 58.2 ms |
| CellApp::TickWitnesses | 12.7 ms | — | — | — | 57.6 ms |
| Witness::Update::Pump | — | — | — | — | — |
| Script.OnTick | 0.41 ms | 0.34 ms | 0.68 ms | 1.74 ms | 7.77 ms |
| Script.EntityTickAll | 0.10 ms | 0.08 ms | 0.18 ms | 0.63 ms | 2.12 ms |
| Script.PublishReplicationAll | 0.31 ms | 0.25 ms | 0.55 ms | 0.90 ms | 6.59 ms |

Key call counts per 120 s run: `Witness::SendEntityUpdate` × 6.76 M,
`Channel::Send` × 1.80 M. Both are the direct product of 100 observers ×
96 peers × 723 ticks — the serialization-per-observer redundancy is the
dominant CPU sink.

**Script侧只贡献 Tick p99 的 3.7%；TickWitnesses 占 Tick mean 的 96.5%。**

### BaseApp (10 Hz)

Tick mean = 0.04 ms, p99 = 0.21 ms. Effectively idle — no optimizations
needed at current scale.

### world_stress summary (100 clients, 10 Hz move, 2 Hz echo)

| Metric | Value |
|---|---|
| auth_latency p50/p95/p99 | 32.7 / 50.7 / 236.9 ms |
| echo_rtt p50/p95/p99 | 135.8 / 1050.7 / 2349.7 ms |
| bytes_rx/s (server→client) | 31.2 KB/s (319 B/s/client) |
| dominant message | 0xF003 (reliable delta) — 97.6% of traffic |

## Documents

| Document | Subsystem | Priority | Profiling notes |
|---|---|---|---|
| [profiler_tracy_integration.md](profiler_tracy_integration.md) | Profiling infrastructure | ~~P0~~ **DONE** | baseline established |
| [distance_lod.md](distance_lod.md) | Witness replication | P0 | TickWitnesses = 96.5% of tick |
| [property_dirty_flags.md](property_dirty_flags.md) | Entity replication | P0 | PublishReplicationAll 0.31 ms mean, spike to 6.6 ms |
| [adaptive_bandwidth.md](adaptive_bandwidth.md) | Witness bandwidth | P0 | echo_rtt p99 > 2 s confirms starvation |
| [group_broadcast.md](group_broadcast.md) | Network fan-out | **P1** ↑ | SendEntityUpdate × 6.76 M — same delta serialized 100× |
| [async_entity_lifecycle.md](async_entity_lifecycle.md) | Dispatcher / entity init | P1 | not visible in current baseline |
| [lazy_baseline.md](lazy_baseline.md) | Baseline serialization | P1 | SendEntityEnter × 9 900, negligible at 100-client scale |
| [incremental_priority_queue.md](incremental_priority_queue.md) | Witness update | P1 | PriorityHeap = 0.054% of tick at 100 clients; scales to P0 at 200 entities |
| [history_window.md](history_window.md) | Replication catch-up | P1 | echo_rtt p99 = 2.3 s indicates snapshot fallback under load |
| [script_publish_gc.md](script_publish_gc.md) | Managed heap / GC | **P1** 🆕 | PublishReplicationAll max 6.6 ms spike; likely GC pause |
| [rangelist_grid.md](rangelist_grid.md) | Spatial partitioning | **P2** ↓ | Space::Tick total < 2 ms/120 s run; not visible at 100-client scale |
| [visibility_culling.md](visibility_culling.md) | AoI filtering | P2 | — |
| [adaptive_ghost_throttle.md](adaptive_ghost_throttle.md) | Cross-cell sync | P2 | TickGhostPump = 7 ms/120 s, negligible |

## Priority Definitions

- **P0** — Required to make 100v100 functional (solves "can it run")
- **P1** — Required to make 100v100 smooth (solves "does it run well")
- **P2** — Polish and advanced features (solves "does it feel good")

`profiler_tracy_integration.md` is complete. Re-run `run_baseline_profile.sh`
after each P0/P1 landing to update the numbers above.
