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
| [network_dispatch_decoupling.md](network_dispatch_decoupling.md) | RUDP receive ↔ dispatch decoupling (Plan B) | 📋 design only | superseded by Plan A (`2c3ced4`) for the 500-client/baseapp goal; revisit if A's per-callback yield runs out at higher per-process loads |
| [channel_send_batching.md](channel_send_batching.md) | RUDP send-side coalescing via descriptor `urgency` flag | 📋 design only | flips `Channel::SendMessage` default to deferred; addresses BaseApp `Channel::Send` 6.30 M calls / 31.8 % CPU at 500 clients; revisit after `network_dispatch_decoupling` lands |

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

## 2-BaseApp / 500-client baseline (`86aceee`, 2026-04-28)

Same workload as the 1-BaseApp v2 run, with `--baseapp-count 2` so each
BaseApp handles 250 clients (production-shape topology). Captures in
`.tmp/prof/baseline-500-2ba/`. Single key finding: the
"single-threaded RUDP ingest" symptom from the 1-BaseApp run is **not
a structural defect** — it was a per-process saturation cliff at the
500-client/proc operating point, and the system below that cliff is
healthy.

### Stress-run health: 1-BaseApp vs 2-BaseApp (same 500 total clients)

| Signal | 1-BA (`b1782e5`) | 2-BA (`86aceee`) | Δ |
|---|---|---|---|
| `unexpected_disc` | 1 302 | **0** | ✅ -100 % |
| `echo_loss` (sent − received) | 24 736 / 87 604 = 28 % | **224 / 102 448 = 0.2 %** | ✅ -99 % |
| `echo_rtt` p50 / p95 / p99 | 940 / 4332 / 4943 ms | **28.6 / 78.2 / 106.3 ms** | ✅ ~33–47× better |
| `auth_latency` p50 / p95 / p99 | 437 / 4584 / 5364 ms | **87.8 / 175.7 / 193.4 ms** | ✅ ~5–28× better |
| `Slow tick` (sum across baseapps) | 16 (max 230 ms) | 5 (max 152 ms) | -69 % |
| `bytes_rx_per_sec` | 1.86 MB/s | **3.20 MB/s** | +72 % |
| `aoi_enter` | 590 314 | 1 045 116 | +77 % |
| `bandwidth deficit` | 0 | 0 | — |
| `EntityID pool exhausted` | 0 | 0 | — |

The 2-BA configuration eliminates every operator-visible symptom and
nearly doubles useful throughput. CellApp doesn't change (still single,
still serving the whole 500-client space).

### Per-BaseApp Tracy at 2-BA 250-clients-each

| Zone | calls (BA0 / BA1) | mean (BA0 / BA1) | max (BA0 / BA1) | % CPU each |
|---|---|---|---|---|
| Tick | 1 951 / 1 952 | 0.55 ms / 0.55 ms | 5.39 ms / 5.67 ms | 0.79 % |
| NetworkInterface::OnRudpReadable | 165 339 / 99 455 | **449 µs / 755 µs** | 55.8 ms / 64.9 ms | **54.3 % / 54.8 %** |
| Channel::Dispatch | 554 472 / 464 569 | 119 µs / 144 µs | 54.9 ms / 64.5 ms | 48.1 % / 48.9 % |
| Channel::HandleMessage | 5.46 M / 5.60 M | 12.0 µs / 11.9 µs | 2.36 ms / 2.00 ms | 48.0 % / 48.7 % |
| Channel::Send | 5.31 M / 5.46 M | 7.34 µs / 7.28 µs | 1.86 ms / 1.11 ms | 28.5 % / 29.0 % |

Compared to 1-BA at 500 clients (`OnRudpReadable` mean 3.07 ms, max
208 ms), the 2-BA per-process numbers are **6.8× / 4.1× lower mean**
and **3.7× / 3.2× lower max**. Per-BaseApp CPU utilization is ~55 %
on the RUDP read path — well clear of the saturation cliff.

## Strategic conclusion (`86aceee`)

There is no structural network bottleneck preventing the 5 000-client
project target. The shape is **straightforward horizontal scaling**:

- Atlas's per-BaseApp comfortable ceiling is around 250–400 clients
  for this action-MMO load profile (10 Hz move + 2 Hz echo + dense AoI).
  500-on-one is past the cliff; 250-on-each is in the linear regime.
- Industry norm for BigWorld-family engines is 500–1000 clients per
  BaseApp; Atlas's lower ceiling reflects (a) per-client property
  density on `StressAvatar` and (b) the still-untuned RUDP read path.
- 5 000 client target → 13–20 BaseApps + 1–3 CellApps (per Space
  cellapp count is bounded by the single-Space target, not by load).
- BaseAppMgr already round-robins logins across BaseApps with
  least-loaded selection (`baseappmgr.cc:500`); no orchestration code
  needs to change.

### Single-point components — health check

LoginApp, BaseAppMgr, and DBApp each run as a single process today
and the architecture has no horizontal-scaling story for them. The
2-BA capture says they're **all idle by a wide margin** at the
5 000-client-equivalent throughput we just measured (3 135 logins
in 138 s = 22.7 logins/s sustained):

| Component | Tick mean | Tick max | % CPU | HandleMessage mean | Verdict |
|---|---|---|---|---|---|
| LoginApp | **1.26 µs** | 10 µs | 0.69 % | 80 µs | ~50× headroom for steady-state 5 000-cli reconnect rate (≈ 250 logins/s at 5 % churn) |
| BaseAppMgr | 1.46 µs | 21 µs | 0.21 % | 16.4 µs | effectively idle |
| DBApp | 220 µs | **9.14 ms** | 1.48 % | 24.9 µs | spikes from periodic DB flush, 60× headroom against the steady auth/checkout rate |

The auth_latency p99 = 193 ms in the 2-BA run is **not** loginapp-bound:
loginapp's local work per login is ~80 µs, the rest is wall-clock
waiting on the chained RPCs (DBApp auth → BaseAppMgr alloc → BaseApp
PrepareLogin). The improvement from 5 364 ms → 193 ms vs the 1-BA run
came entirely from baseapp's PrepareLogin getting unstuck — not from
loginapp speeding up.

Single-point loginapp / baseappmgr is a **HA concern, not a perf
concern**. Failover / hot-spare is a future deployment-architecture
question; performance-wise the single instance has more than enough
headroom for the project target.

**Open questions still worth investigating, but no longer urgent:**

1. **Per-BaseApp ceiling tuning** — can we push 250-cli/proc to
   400-cli/proc by shaving the OnRudpReadable mean (currently 449 µs
   at 250 clients vs 144 µs at 200 clients on a shared baseapp — the
   per-call work is super-linear in client count). Open
   `baseapp_86aceee_*.tracy` in `tracy-profiler` to see what's inside.
2. **Slow-tick magnitude** — the 5 remaining slow-ticks (138–152 ms)
   are still 2× the 67 ms budget. Bursty rather than sustained, but
   worth understanding before sustained 5k-client deployments.
3. **CellApp scaling** — at 5 000 clients in many small spaces, single
   cellapp suffices; at 5 000 clients in a few large 100v100 battles,
   each Space pins a single cellapp and there's no horizontal scaling
   inside a Space. Re-run with `--cellapp-count 2 --space-count 2` to
   establish the multi-cellapp curve.
4. **DBApp tick spikes** — Tick max 9.14 ms is 14× the mean (220 µs);
   likely the periodic counter persist (`EntityIdAllocator::Persist`).
   At higher login rates this could chain into auth_latency tail. Worth
   checking the 5 000-client reconnect-storm scenario.

These are scaling-curve refinements, not blockers. Update this doc
the next time a single zone exceeds its budget at the chosen
production topology.

## Priority Definitions

- **P0** — Required to make 100v100 functional ("can it run")
- **P1** — Required to make 100v100 smooth ("does it run well")
- **P2** — Polish ("does it feel good")

At the current data point, every P0/P1 in the list has either shipped
or fallen below its evidence threshold. **Do not** add new tasks to
this directory based on the headings alone — add them after a fresh
capture shows a specific zone overrunning its budget, and link the
capture path in the "Profiling evidence" section of the new doc.
