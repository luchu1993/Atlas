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

## 500-client baseline (`b1782e5`, 2026-04-28)

Single-baseapp / single-cellapp / single-Space, 500 clients × 120 s
(`run_20260428-190132.log`, `cellapp_b1782e5_*.tracy`,
`baseapp_b1782e5_*.tracy`). This is **above the comfortable per-baseapp
ceiling** for action-MMO workloads; we run it deliberately to surface
the first cluster-scale bottleneck. Test-bed config bugs surfaced by
the v1 (`66a278c`) run were fixed in `228e67a` (EntityID pool
watermarks) and `b1782e5` (witness bandwidth budget retune); v2
numbers are reported here.

### CellApp under 500-client load

| Zone | calls | mean | max | total | % CPU |
|---|---|---|---|---|---|
| Tick | 1 951 | **7.27 ms** | **68.3 ms** | 14.2 s | 7.3 % |
| CellApp::TickWitnesses | 1 951 | 5.52 ms | 59.7 ms | 10.8 s | 5.5 % |
| Witness::Update | 787 291 | 12.1 µs | 16.6 ms | 9.49 s | 4.9 % |
| Witness::SendEntityUpdate | 8 410 065 | 285 ns | 15.3 ms | 2.39 s | 1.2 % |
| Script.OnTick | 1 951 | 1.42 ms | 9.32 ms | 2.77 s | 1.4 % |
| Script.PublishReplicationAll | 1 951 | 1.10 ms | 7.20 ms | 2.14 s | 1.1 % |
| NetworkInterface::OnRudpReadable | 115 438 | 144 µs | 28.4 ms | 16.6 s | 8.5 % |

CellApp tick mean 7.3 ms = 7 % of the 100 ms budget. Tick max **68 ms
= 68 % of budget** — getting close but no slow-tick yet. Headroom
remains, but `Witness::Update::Pump` max climbed to 15 ms (vs 1.4 ms
at 200-client), reflecting bursty per-witness work when AoI density
spikes.

### BaseApp under 500-client load — **the structural bottleneck**

| Zone | calls | mean | max | total | % CPU |
|---|---|---|---|---|---|
| Tick | 1 952 | 0.73 ms | 7.26 ms | 1.42 s | 1.0 % |
| **NetworkInterface::OnRudpReadable** | 30 669 | **3.07 ms** | **208 ms** | 94.2 s | **68.9 %** |
| Channel::Dispatch | 650 058 | 117 µs | 204 ms | 76.2 s | **55.7 %** |
| Channel::HandleMessage | 6 592 504 | 11.5 µs | 53.9 ms | 76.0 s | **55.6 %** |
| Channel::Send | 6 296 525 | 6.91 µs | 53.9 ms | 43.5 s | **31.8 %** |

`OnRudpReadable` is doing **3 ms per call on average, 208 ms worst
case**. With every callback running on the dispatcher thread, the
`Slow tick` warnings (16 events, 230 ms / 210 ms / 160 ms / 157 ms /
148 ms over a 67 ms budget) are **direct consequences of the single-
threaded RUDP read path being unable to clear its queue between ticks**.

### Stress-run health (v2)

| Signal | v1 (`66a278c`, no fix) | v2 (`b1782e5`, fixed) |
|---|---|---|
| `EntityID pool exhausted` | 387 | **0** |
| `bandwidth deficit` | 103 | **0** |
| `[GC-in-tick]` | 1 | 1 |
| `Slow tick` (baseapp) | 14 | 16 |
| online_at_end | 449 | **483** |
| auth_success | 3 484 | 3 547 |
| entity_transferred | 3 077 | **3 511** |
| unexpected_disc | 1 424 | 1 302 |
| echo_rtt p50 / p95 / p99 | 384 / 4111 / 5203 ms | **940** / 4332 / 4943 ms |
| auth_latency p50 / p95 / p99 | 392 / 4219 / 5042 ms | 437 / 4584 / 5364 ms |
| bytes_rx_per_sec | 1.64 MB/s | 1.86 MB/s |

**A and D worked.** v2's `[GC-in-tick]` count (1) and `Slow tick`
count (16) are unchanged because both fixes targeted v1 noise, not
the structural baseapp bottleneck. The echo_rtt p50 doubled from 384
to 940 ms because more traffic is now flowing — the previous
1.6 MB/tick fleet cap was implicitly suppressing demand, masking how
saturated the baseapp dispatcher actually was.

## Where the next bottleneck is (`b1782e5`, NOT in the doc list)

The 500-client v2 capture pinpoints **single-threaded RUDP ingest on
baseapp** as the structural ceiling:

- `OnRudpReadable` mean **3.07 ms / call** at 500 clients — 24× worse
  than the 200-client baseline (128 µs).
- Total CPU on the RUDP read path: **68.9 %** of one baseapp core
  across the capture (Channel::Dispatch + HandleMessage are nested,
  not additive — they contribute to the same 68.9 %).
- 16 `Slow tick` warnings on baseapp (max 230 ms vs 67 ms budget)
  trace directly to inbound bursts blocking dispatch from yielding.
- Adding more BaseApps (`run_world_stress.py --baseapp-count N`)
  partitions sessions across processes and is the production scaling
  story; **per-process** the read-path work needs to either move off
  the dispatcher thread or accept multi-message vectored reads.

Concrete options to investigate before any code change:

1. **Multi-baseapp deployment first** — re-run baseline with
   `--baseapp-count 2` (250 clients/proc) to establish a per-process
   curve. Action-MMO industry norm is 500–1000 clients per BaseApp;
   our 500-on-one is already past that.
2. **OnRudpReadable internals** — open `cellapp_b1782e5_*.tracy` /
   `baseapp_b1782e5_*.tracy` in tracy-profiler to see what's inside
   the 3 ms mean — is it ACK processing, fragment reassembly, queue
   walks, or syscall overhead?
3. **Yield budget** — `Slow tick` fires when the dispatcher loop runs
   for too long without returning to the tick driver; a per-callback
   wall-clock budget (similar to the existing witness send budget)
   could bound the worst-case stall.

Decide architecture direction (vectored reads vs worker threads vs
per-tick yield) only after the per-process curve from option 1 says
which one buys headroom.

## Priority Definitions

- **P0** — Required to make 100v100 functional ("can it run")
- **P1** — Required to make 100v100 smooth ("does it run well")
- **P2** — Polish ("does it feel good")

At the current data point, every P0/P1 in the list has either shipped
or fallen below its evidence threshold. **Do not** add new tasks to
this directory based on the headings alone — add them after a fresh
capture shows a specific zone overrunning its budget, and link the
capture path in the "Profiling evidence" section of the new doc.
