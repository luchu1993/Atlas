# Optimization Roadmap

Per-subsystem optimization docs for Atlas Engine. Each doc describes
the current shipped design (or a deferred future-work proposal) and
the trigger for revisiting it.

> **Read this first.** Numbers in this doc drive priority. Before
> opening any task in this directory, re-run `run_baseline_profile.sh`
> and confirm the bottleneck the doc claims is *still* visible. Most
> of the originally-prioritised items have shipped; do not pick from
> the list and run — drive from a fresh capture.

## Target scenario

**100v100 large-scale battle**: 200 entities in a single Space, all
within AoI range, complex combat logic in C# scripts. This is the
design ceiling Atlas must support comfortably. The cluster target
is **5 000 concurrent clients** across 13–20 BaseApps + 1–3
CellApps; per-Space scaling is bounded by the single-Space target,
not by load.

## Current baseline (`3ab62ca`, 2026-04-30, 500 cli × 120 s, 1-BA / 1-CA)

Captures in `.tmp/prof/baseline/{baseapp,cellapp,…}_3ab62ca_*.tracy`.
Single-baseapp / single-cellapp / single-Space, deliberately above
the comfortable per-baseapp ceiling so cluster-scale bottlenecks
surface.

### CellApp (10 Hz, 100 ms tick budget)

| Zone | mean | max | total | % CPU |
|---|---|---|---|---|
| Tick | 9.2 ms | 28.2 ms | 18.0 s | 9.2 % |
| CellApp::TickWitnesses | 7.6 ms | 17.5 ms | 14.8 s | 7.6 % |
| Witness::Update::Pump | 5.2 µs | 670 µs | 5.6 s | 2.9 % |
| Witness::Event::Send | 322 ns | 652 µs | 2.54 s | 1.3 % |
| Witness::Update::PriorityHeap | 3.3 µs | 3.8 ms | 3.54 s | 1.8 % |
| NetworkInterface::OnRudpReadable | 244 µs | 10.7 ms | 18.8 s | 9.6 % |

Tick mean 9 % of budget; max 28 % — comfortable headroom. Per-call
`Witness::Event::Send` (322 ns) is below the 200-cli pre-cache
baseline (335 ns), so the scale-induced retreat seen earlier is
gone.

### BaseApp (10 Hz, 100 ms tick budget)

| Zone | calls | mean | total | % CPU |
|---|---|---|---|---|
| Tick | 1 952 | 10.9 ms | 21.3 s | — |
| Channel::HandleMessage | 20.26 M | 2.75 µs | 55.8 s | — |
| Channel::Send | 760 k | 7.49 µs | 5.7 s | — |
| OnRudpReadable | 81 660 | 808 µs | 66.0 s | — |

Send-side coalescing brought BaseApp `Channel::Send` calls from
6.30 M (pre-batching) to under a million while raising effective
throughput; see [channel_send_batching.md](channel_send_batching.md).

### Stress-run health

| Signal | Count |
|---|---|
| `[GC-in-tick]` warnings | 0 |
| `bandwidth deficit` warnings | 0 |
| `Slow tick` warnings | 0 |
| `unexpected_disc` | 0 |
| `timeout_fail` | 0 |
| `login_fail` | 0 |
| `cell_ready / entity_transferred` | 3 183 / 3 183 (100 %) |
| `online_at_end` | 482 |
| `echo_loss` | 158 / 103 042 (0.15 %) |
| `echo_rtt` p50 / p95 / p99 | 18.7 / 47.7 / 60.7 ms |
| `auth_latency` p50 / p95 / p99 | 73.5 / 191.6 / 207.9 ms |

500-cli on 1-BA / 1-CA now runs the way 200-cli ran before every
optimization landed: every failure-class signal is zero.

## Documents

Status legend: ✅ shipped · 🔵 deferred (future scale) · ⚪ open
(game-design dependent).

| Document | Subsystem | Status |
|---|---|---|
| [profiler_tracy_integration.md](profiler_tracy_integration.md) | Profiling infrastructure | ✅ |
| [distance_lod.md](distance_lod.md) | Witness replication LOD bands | ✅ |
| [property_dirty_flags.md](property_dirty_flags.md) | Codegen dirty-flag tracking | ✅ |
| [adaptive_bandwidth.md](adaptive_bandwidth.md) | Demand-based witness bandwidth | ✅ |
| [group_broadcast.md](group_broadcast.md) | Envelope cache for shared deltas | ✅ |
| [script_publish_gc.md](script_publish_gc.md) | C# replication GC pressure | ✅ (round 1) |
| [channel_send_batching.md](channel_send_batching.md) | RUDP send-side coalescing | ✅ |
| [witness_channel_cache.md](witness_channel_cache.md) | Witness send-path channel cache | ✅ |
| [incremental_priority_queue.md](incremental_priority_queue.md) | Witness priority heap incremental update | 🔵 |
| [lazy_baseline.md](lazy_baseline.md) | Compact / lazy baseline snapshot | 🔵 |
| [rangelist_grid.md](rangelist_grid.md) | Spatial grid overlay on RangeList | 🔵 |
| [adaptive_ghost_throttle.md](adaptive_ghost_throttle.md) | Velocity-adaptive ghost interval | 🔵 |
| [visibility_culling.md](visibility_culling.md) | Pluggable AoI visibility filter | ⚪ |

## Strategic conclusion

There is no structural network bottleneck preventing the 5 000-cli
project target. The shape is **straightforward horizontal scaling**:

- Atlas's per-BaseApp comfortable ceiling is around 250–400 clients
  for this action-MMO load profile (10 Hz move + 2 Hz echo + dense
  AoI). Industry norm for BigWorld-family engines is 500–1 000
  clients per BaseApp; Atlas's lower ceiling reflects per-client
  property density on `StressAvatar` and the still-untuned RUDP
  read path.
- 5 000-client target → 13–20 BaseApps + 1–3 CellApps. Per-Space
  cellapp count is bounded by the single-Space target, not by load.
- BaseAppMgr already round-robins logins with least-loaded selection
  (`baseappmgr.cc:500`); no orchestration code needs to change.
- Single-instance LoginApp / BaseAppMgr / DBApp run at < 1.5 % CPU
  with 50–60× headroom for the steady-state 5 000-cli reconnect
  rate. Single-instance is an HA concern (failover / hot-spare),
  not a perf concern.

### Open questions (not blockers)

1. **Per-BaseApp ceiling tuning.** Push 250-cli/proc to 400-cli/proc
   by shaving the `OnRudpReadable` mean — at 250 clients the per-
   call work is super-linear in client count.
2. **Slow-tick magnitude.** Remaining bursty slow-ticks (138–152 ms,
   ~2× the 67 ms BaseApp budget) are worth understanding before
   sustained 5 000-cli deployments.
3. **Multi-cellapp scaling.** At 5 000 clients in many small spaces
   single cellapp suffices; in a few large 100v100 battles each
   Space pins one cellapp. Re-run with
   `--cellapp-count 2 --space-count 2` to establish the multi-
   cellapp curve.
4. **DBApp tick spikes.** Tick max 9.14 ms is 14× the mean (220 µs);
   likely the periodic counter persist
   (`EntityIdAllocator::Persist`). Could chain into `auth_latency`
   tail under high reconnect-storm load. Worth checking the
   5 000-cli reconnect-storm scenario.

## Priority definitions

- **P0** — Required to make 100v100 functional ("can it run").
- **P1** — Required to make 100v100 smooth ("does it run well").
- **P2** — Polish ("does it feel good").

Every P0 / P1 item is shipped or below its evidence threshold. Do
**not** add new tasks based on these headings alone — add them
after a fresh capture shows a specific zone overrunning its budget,
and link the capture path in the new doc's evidence section.
