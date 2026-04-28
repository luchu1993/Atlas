# Optimization Roadmap

This directory documents planned optimizations for Atlas Engine, organized by
subsystem. Each document describes the current bottleneck, proposed solution,
expected impact, and implementation notes.

> **Read this first.** The numbers in this document drive priority. Before
> opening any task in this directory, re-run `run_baseline_profile.sh` and
> confirm the bottleneck the doc claims is *still* visible — most of the
> P0/P1 items shipped between `b70b0ad` and `dfed976`, and the picture has
> changed substantially. **Do not pick optimizations from the list and run;
> drive from a fresh capture.**

## Target Scenario

**100v100 large-scale battle**: 200 entities in a single Space, all within AoI
range, with complex combat logic running in C# scripts. This is the design
ceiling Atlas must support comfortably.

## Profiling Baseline (`dfed976`, 2026-04-28, 200 clients × 120 s)

Captured by `run_baseline_profile.sh` against the `profile` build (200
clients, 120 s run + ~17 s ramp/cooldown → ~137 s capture window). Tracy
files in `.tmp/prof/baseline/cellapp_dfed976_20260428-155158.tracy` and
the `baseapp_*` companion. Numbers below are from `tracy-csvexport`
(mean / max / total). Tail percentiles aren't in the exporter today —
open the trace in `tracy-profiler` for p95 / p99.

### CellApp (10 Hz, 100 ms tick budget)

| Zone | calls | mean | max | total | % CPU |
|---|---|---|---|---|---|
| Tick | 1 952 | 2.83 ms | 14.2 ms | 5.53 s | 4.05 % |
| CellApp::OnTickComplete | 1 952 | 2.80 ms | 14.2 ms | 5.46 s | 4.00 % |
| CellApp::TickWitnesses | 1 952 | 1.63 ms | 5.33 ms | 3.18 s | 2.33 % |
| Witness::Update | 428 041 | 6.34 µs | 2.70 ms | 2.72 s | 1.99 % |
| Witness::Update::Transitions | 428 041 | 3.10 µs | 2.70 ms | 1.33 s | 0.97 % |
| Witness::Update::Pump | 428 041 | 1.93 µs | 1.44 ms | 0.83 s | 0.60 % |
| Witness::Update::PriorityHeap | 428 041 | 1.27 µs | 1.54 ms | 0.54 s | 0.40 % |
| Witness::SendEntityUpdate | 1 931 841 | 389 ns | 1.43 ms | 0.75 s | 0.55 % |
| Witness::SendEntityEnter | 284 983 | 1.63 µs | 1.43 ms | 0.46 s | 0.34 % |
| Witness::SendEntityLeave | 426 114 | 1.30 µs | 2.60 ms | 0.56 s | 0.41 % |
| Witness::Event::Build | 56 166 | 662 ns | 71 µs | 37 ms | 0.027 % |
| Witness::Event::Send | 984 598 | 226 ns | 255 µs | 222 ms | 0.16 % |
| Witness::Vol::Send | 1 220 523 | 143 ns | 308 µs | 175 ms | 0.13 % |
| Script.OnTick | 1 952 | 987 µs | 10.18 ms | 1.93 s | 1.41 % |
| Script.PublishReplicationAll | 1 952 | 701 µs | 8.63 ms | 1.37 s | 1.00 % |
| Script.EntityTickAll | 1 952 | 283 µs | 6.96 ms | 0.55 s | 0.40 % |
| Space::Tick | 1 946 | 13 µs | 0.62 ms | 26 ms | 0.019 % |
| Channel::Send | 124 207 | 9.58 µs | 4.52 ms | 1.19 s | 0.87 % |
| NetworkInterface::OnRudpReadable | 86 512 | 69 µs | 10.0 ms | 6.01 s | 4.40 % |

**Health summary (cellapp).** Tick mean is 2.8 % of the 10 Hz budget;
worst tick is 14.2 ms = 14 % of budget. Across 137 s the run logged
**zero** `[GC-in-tick]`, `bandwidth deficit`, `Slow tick`, or snapshot-
fallback warnings. CellApp **is not the current bottleneck** at 200
clients. Inside `TickWitnesses` (still 58 % of mean tick), per-witness
work is `Witness::Update` ≈ 6.3 µs amortised over 219 active witnesses
per tick — there is no single dominant cost left to attack.

### BaseApp (10 Hz, 100 ms tick budget)

The historical claim that BaseApp is "effectively idle" no longer
holds. Tick wall-clock is healthy, but the network fan-out path
absorbs the majority of baseapp CPU.

| Zone | calls | mean | max | total | % CPU |
|---|---|---|---|---|---|
| Tick | 1 952 | 3.87 ms | 9.05 ms | 7.56 s | 5.52 % |
| NetworkInterface::OnRudpReadable | 105 248 | 331 µs | 29.0 ms | 34.9 s | **25.5 %** |
| Channel::Dispatch | 394 562 | 75 µs | 27.0 ms | 29.7 s | **21.7 %** |
| Channel::HandleMessage | 3 307 111 | 8.95 µs | 5.20 ms | 29.6 s | **21.6 %** |
| Channel::Send | 2 445 172 | 8.39 µs | 5.19 ms | 20.5 s | **15.0 %** |
| Script.OnTick | 1 952 | 102 µs | 0.84 ms | 200 ms | 0.15 % |
| Script.PublishReplicationAll | 1 952 | 10.2 µs | 0.40 ms | 20 ms | 0.014 % |
| Script.EntityTickAll | 1 952 | 88.9 µs | 0.50 ms | 174 ms | 0.13 % |

**Health summary (baseapp).** Tick mean 3.87 ms (3.9 % budget); max
9.05 ms (9 % budget). But ~80 % of CPU is in the network layer:
3.31 M `Channel::HandleMessage` + 2.45 M `Channel::Send` calls = the
cellapp delta channel fanning out to 200 client channels (one cellapp
envelope → ≤ 200 baseapp sends). `OnRudpReadable` max 29 ms indicates
intermittent socket-burst stalls; no tick-budget violation yet, but the
slope is what breaks first when client count rises.

### Stress run health

`/.tmp/world-stress/20260428-155158/`:

| Signal | Count |
|---|---|
| `[GC-in-tick]` warnings | 0 |
| `bandwidth deficit` warnings | 0 |
| `Slow tick` warnings | 0 |
| Snapshot fallback log | 0 |
| stderr errors / unexpected warnings | 0 |

Earlier baseline numbers in this README claimed `echo_rtt p99 = 2.35 s`
(commit `b70b0ad`); that signal is **gone** at 200 clients. The
world_stress harness stdout for this run was not teed to file, so
fresh client-side RTT percentiles aren't available here — re-run
baseline with stdout capture before relying on those numbers.

## What's already shipped (post-`b70b0ad`)

These changes drove the cellapp tick from `b70b0ad`'s 13.1 ms / 58.2 ms
(mean / max) down to today's 2.83 ms / 14.2 ms:

| Optimization | Commit | Effect |
|---|---|---|
| Distance LOD | `54bd9df` | Far-band updates 1× / 6 ticks; reduced per-observer fan-out |
| Demand-based bandwidth allocator | `50ac6b8` | Eliminated `bandwidth deficit` warnings; per-observer demand sized to peer count + NIC cap |
| Envelope cache (group broadcast) | `0c1e755` | `Witness::SendEntityUpdate` calls 6.76 M → 1.93 M (-71 %); total CPU -91 % |
| C# GC pass round 1 | `f382f08` | Eliminated `IEnumerable<T>` boxing in delta-sync codegen; recurring `[GC-in-tick]` gone |
| Property dirty-flag tracking | codegen `_dirtyFlags` + sectionMask | Only changed properties cross the wire |
| Per-tick witness send batching | `0dced2f` | Cellapp `Channel::Send` calls 1.80 M → 124 k |
| Incremental transition lists | `4a6fcf7` | `Witness::Update::Transitions` is O(Δ), not O(N) |

## Documents

Status legend: ✅ shipped · 🟡 unjustified at current data · 🔵 deferred
(future scale only) · ⚪ open / game-design dependent.

| Document | Subsystem | Status | Note from current data |
|---|---|---|---|
| [profiler_tracy_integration.md](profiler_tracy_integration.md) | Profiling infrastructure | ✅ | baseline pipeline established |
| [distance_lod.md](distance_lod.md) | Witness replication | ✅ | `54bd9df` |
| [property_dirty_flags.md](property_dirty_flags.md) | Entity replication | ✅ | shipped via codegen, not via the proposed C++ bitset path |
| [adaptive_bandwidth.md](adaptive_bandwidth.md) | Witness bandwidth | ✅ | `50ac6b8`; zero `bandwidth deficit` warnings in current run |
| [group_broadcast.md](group_broadcast.md) | Network fan-out | ✅ | `0c1e755` envelope cache |
| [script_publish_gc.md](script_publish_gc.md) | Managed heap / GC | ✅ (round 1) | `f382f08`; `Script.PublishReplicationAll` mean 701 µs / max 8.6 ms — slightly above the round-1 verification numbers but no recurring `[GC-in-tick]` |
| [history_window.md](history_window.md) | Replication catch-up | 🟡 | original signal (`echo_rtt p99 = 2.3 s`) is from `b70b0ad` and gone. Zero snapshot fallbacks in the current capture; re-evaluate after a higher-load capture exposes the symptom. |
| [incremental_priority_queue.md](incremental_priority_queue.md) | Witness update | 🔵 | `Witness::Update::PriorityHeap` is 0.40 % of CPU at 200 clients; revisit if it scales above ~5 % |
| [async_entity_lifecycle.md](async_entity_lifecycle.md) | Dispatcher / entity init | 🟡 | no slow-tick lifecycle storms in current capture; re-instrument the spawn path before deciding |
| [lazy_baseline.md](lazy_baseline.md) | Baseline serialization | 🔵 | `SendEntityEnter` total 0.46 s / 285 k calls = 0.34 % CPU; `cached_*_envelope` already covers deltas (Tactic 3 spirit). Defer until a denser AoI scenario shows enter-storm cost. |
| [rangelist_grid.md](rangelist_grid.md) | Spatial partitioning | 🔵 | `Space::Tick` is 0.019 % of CPU; not on any visible critical path |
| [visibility_culling.md](visibility_culling.md) | AoI filtering | ⚪ | game-design dependent (fog-of-war / team filtering); defer until combat specs land |
| [adaptive_ghost_throttle.md](adaptive_ghost_throttle.md) | Cross-cell sync | 🔵 | `CellApp::TickGhostPump` is 0.045 % of CPU — single-cellapp deployment in the current baseline |

## Where the next bottleneck is (NOT in the list above)

The 200-client capture's loudest signal is **baseapp network fan-out**:

- 3.31 M `Channel::HandleMessage` × 8.95 µs ≈ 30 s of CPU per 137 s window (~22 % of one core)
- 2.45 M `Channel::Send` × 8.4 µs ≈ 21 s (~15 %)
- `OnRudpReadable` max 29 ms — intermittent socket-burst stalls, not yet a tick-budget violation but visible

This is throughput-bound, not latency-bound, at 200 clients. The path
to 5 000 clients (project target) goes straight through it. Any new
optimization work should start by:

1. Re-running `run_baseline_profile.sh` with `--clients 500` (or higher)
   and **teeing world_stress stdout** so client RTT percentiles are
   visible.
2. Checking baseapp tick max + `OnRudpReadable` max as load rises;
   `Channel::HandleMessage` count grows linearly with client count.
3. Picking a target only after the capture identifies which of
   {RUDP read path, dispatch dispatch, per-channel send, fan-out
   amplification} actually dominates at the next scale step.

## Priority Definitions

- **P0** — Required to make 100v100 functional ("can it run")
- **P1** — Required to make 100v100 smooth ("does it run well")
- **P2** — Polish ("does it feel good")

At the current data point, every P0/P1 in the list has either shipped
or fallen below its evidence threshold. **Do not** add new tasks to
this directory based on the headings alone — add them after a fresh
capture shows a specific zone overrunning its budget, and link the
capture path in the "Profiling evidence" section of the new doc.
