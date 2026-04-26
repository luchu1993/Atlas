# Tracy Profiler — Usage Guide

This is the practical "how do I actually use Tracy on Atlas" guide. For the
operator runbook (build presets, deploy, multi-process), see
[`profiling.md`](profiling.md). For the design rationale (why Tracy, why this
shape), see [`../optimization/profiler_tracy_integration.md`](../optimization/profiler_tracy_integration.md).

## What Tracy gives you

Tracy is a sampling-and-instrumentation hybrid profiler. Atlas wires it as an
**instrumentation** profiler — every named region the engine emits
(`ATLAS_PROFILE_ZONE_N(...)`) becomes a row on Tracy's timeline. The viewer
also captures system-level samples (CPU context switches, kernel calls) on
top of that, so you see Atlas's logical structure and the OS's underlying
view side by side.

Concretely, after attaching the viewer to a running CellApp you get:

- **Frame timeline** with one bar per logical tick (`OpenWorldTick`,
  `<process_name>.Tick`, etc.). Hovering a bar shows its duration; clicking
  zooms in.
- **Per-frame zone tree**: `Tick → CellApp::OnTickComplete → TickWitnesses →
  Witness::Update → Witness::Update::Pump → SendEntityUpdate → …`. Drill in
  to the level where the cost lives.
- **Plots**: `TickWorkMs`, `BytesIn`, `BytesOut` charted against the same
  time axis. A spike in `TickWorkMs` lines up directly with whatever zone
  was open when it happened.
- **Memory tracking**: per-pool streams (`TimerNode`, future Atlas pools)
  plus the global heap. The Memory tab shows allocation/free counts, current
  usage, and the unfreed list — useful for leak hunting.
- **Lock contention**: `std::mutex` instances wrapped via
  `ATLAS_PROFILE_LOCKABLE` show their wait/hold ranges on the timeline.
- **C# zones interleaved**: `Script.OnTick` and any
  `Atlas.Diagnostics.Profiler.Zone(...)` call from managed code shows up on
  the same timeline as the C++ zones — unified view.

What Tracy **doesn't** give you out of the box:

- **Cross-process views**: one viewer attaches to one process at a time. For
  a 4-process cluster you switch between attached sessions. The viewer's
  Discover dialog lists all of them; reconnecting is one click.
- **Distributed traces**: no notion of "this RPC sent from BaseApp arrived
  at CellApp". That's the OpenTelemetry work intentionally deferred from
  Phase 5b of the integration plan.
- **Continuous capture**: by default Tracy buffers everything in memory.
  Long sessions accumulate hundreds of MB. Use **Save trace** before
  closing to keep history.

## Get the viewer

Tracy ships pre-built viewer binaries on its GitHub releases:

```
https://github.com/wolfpld/tracy/releases
```

Atlas pins Tracy native to **0.13.1** (see `cmake/Dependencies.cmake`). The
viewer's wire protocol must match that minor version exactly — using a
0.12.x viewer against a 0.13.1 client connects but silently drops zones.
Download `Tracy-0.13.1.7z` (Windows) or `Tracy-0.13.1.tar.gz` (Linux) and
unpack the GUI executable somewhere in `PATH`.

## Attach to a server process

### Step 1 — build with the profiler enabled

Either preset works:

```bash
cmake --preset debug              # day-to-day
cmake --preset profile-release    # production-shaped
cmake --build build/<preset> --config Debug
```

The `release` preset has profiler **off** by design — production binaries
ship without Tracy. `profile-release` is what you want for any "is this
faster" experiment.

### Step 2 — run the server

```bash
bin/profile-release/server/atlas_cellapp.exe --config server.json
```

By default Atlas's Tracy client runs in **on-demand** mode
(`TRACY_ON_DEMAND=ON`). The process consumes ~0 CPU on profiler infrastructure
until a viewer attaches. No flush, no broadcast, no buffering. This is
deliberate: we want the option to leave instrumentation in production
binaries (when using `profile-release` rather than `release`) without
running profiler overhead 24/7.

### Step 3 — connect the viewer

1. Launch `Tracy.exe` (or `tracy-profiler` on Linux).
2. Click **Connect** in the toolbar.
3. The address field defaults to `127.0.0.1`. Leave it. Port `8086` is the
   default Tracy listen port.
4. Click **Connect**.

The first frame appears in <100 ms. Anything before the moment of attach
is gone — Tracy's on-demand mode buffers nothing in advance. Plan around
this: if you're chasing a startup-time issue, build with
`-DATLAS_PROFILER_ON_DEMAND=OFF` instead, which buffers from process start.

### Multiple servers on one host

Tracy auto-falls-back the listen port: process 1 takes 8086, process 2
takes 8087, etc. The viewer's **Discover** button (next to Connect) scans
the local range and lists every active session by program name. machined
spawns each Atlas process with its own client; you don't need to inject
`TRACY_PORT` env vars.

When two CellApps are running, you'll see two entries:

```
Discover ─┬─ atlas_cellapp [PID 12345] @ 127.0.0.1:8086
          └─ atlas_cellapp [PID 12346] @ 127.0.0.1:8087
```

Click one to attach. Switch by clicking **Disconnect** then picking the
other. State per session is independent.

## Read the timeline

### Frames row (top of the screen)

The thick bars at the top are **frames**. Atlas emits one named frame per
tick (`<process_name>.Tick` by default, `DungeonTick` if `ServerConfig::frame_name`
is overridden). Each bar's width is the tick's wall duration; the colour
encodes "is this longer than the configured frame budget".

Slow ticks show up as red. Click one to zoom into its zone tree.

### Zone tree (main timeline)

Each row is a thread; each coloured block is a zone with a duration. The
top-level Atlas zone is `Tick`. Underneath you'll typically see (for
CellApp):

```
Tick
├─ CellApp::OnEndOfTick
│   ├─ CellApp::TickGhostPump
│   ├─ CellApp::TickOffloadChecker
│   └─ CellApp::TickOffloadAckTimeouts
├─ Updatables
├─ CellApp::OnTickComplete
│   ├─ Script.OnTick                ← C# combat / movement logic
│   ├─ CellApp::TickControllers
│   │   └─ Space::Tick (× N spaces)
│   ├─ CellApp::TickWitnesses
│   │   └─ Witness::Update (× N entities with witness)
│   │       ├─ Witness::Update::Transitions
│   │       ├─ Witness::Update::PriorityHeap
│   │       └─ Witness::Update::Pump
│   │           └─ Witness::SendEntityUpdate (× N peers)
│   ├─ CellApp::TickBackupPump
│   └─ CellApp::TickClientBaselinePump
```

If a zone has a `Channel::Send` underneath it, that's the network outbound
cost; `BytesOut` plot below the timeline shows the size at that moment.

### Plots (below the timeline)

Plots stay in lock-step with frames. Atlas emits:

| Plot | Source | Typical reading |
|---|---|---|
| `TickWorkMs` | Per-tick work time | should be well under `1000/update_hertz` (33 ms at 30 Hz) |
| `BytesOut` | Per-packet outbound | spikes correlate with `Witness::Update::Pump` zones |
| `BytesIn` | Per-packet inbound | spikes correlate with `Channel::Dispatch` zones |

Hover any point to read the value at that moment. The **Edit plots** menu
controls colour, formatting (e.g. show as memory rather than raw number),
and scale.

### Find / Find Zone

`Ctrl-F` opens the zone search. Type any zone name to filter the list.
**Find Zone** (separate dialog) shows the histogram of one zone's duration
across the trace — useful for spotting outliers (`Witness::Update::Pump`
mostly 0.5 ms, but tail at 8 ms? click the outlier, it jumps to that moment).

## Memory tab

Atlas reports two kinds of allocations:

1. **Global heap** — every `new` / `delete` (and the C++17 / C++14 sized /
   aligned variants), routed through `atlas::HeapAlloc`. Reported as
   anonymous events.
2. **Per-pool** — every `PoolAllocator(name, …)`. Reported with the pool's
   name. Today's pools include `TimerNode`; new pools get their name
   automatically when their constructor passes a stable pointer.

The Memory tab shows:

- **Total allocated / freed bytes** over time
- **Per-allocator chart** — one stream per pool name plus "anonymous" for
  the global heap
- **Active allocations list** — what's still allocated at the cursor's
  timestamp. Useful for leak detection: pause the cluster, jump to a known
  steady state, look for anything that should have been freed but wasn't.

### Picking a pool name

`PoolAllocator(pool_name, …)` requires `pool_name` to be a string literal
or otherwise statically-allocated pointer. Tracy keys per-pool memory
streams by pointer identity, not value, so a `std::string::c_str()` from
a stack variable would corrupt the trace. Atlas's existing pools all pass
literals; new ones should too.

## C# zones in the same trace

The server's C# layer (Atlas.Runtime) installs `TracyProfilerBackend`
during `Lifecycle.DoEngineInit`. After that, every `Profiler.Zone(...)` /
`Profiler.ZoneN(name)` from managed code emits to the same Tracy client
the C++ side uses. Specifically:

- `Script.OnTick` zone wraps the entire managed tick body
- (Future) generator-emitted property apply zones will appear under
  `Script.OnTick` once the def-codegen pass adds them

The zones interleave with C++ zones contiguously. There is no separate
"managed" pane — the trace looks the same as if Atlas were a single C++
program that happened to call into more C++.

If managed zones are missing while C++ zones are present, the most likely
cause is that `Lifecycle.DoEngineInit` hasn't run yet (the runtime hasn't
booted) or the install was rejected (a non-null backend was already
present). Check `ATLAS_LOG_INFO` output for `"Tracy profiler backend
already installed"`.

## Lock contention

Wrap a contended mutex with `ATLAS_PROFILE_LOCKABLE(std::mutex, my_lock)`
instead of `std::mutex my_lock` to add it to Tracy's contention timeline.
The wrapped lock behaves identically at the source level (same `lock()`,
`unlock()`, `lock_guard` interaction) but reports wait / hold ranges to
the viewer.

The Lock tab then shows:

- Which threads contended on this lock
- How long each thread waited
- Hold-time distribution

Useful for verifying that a hot-path mutex isn't quietly serialising work
you assumed was parallel.

`ATLAS_PROFILE_LOCK_MARK(var)` emits a sync point against the lock without
actually claiming it — useful when you've manually serialised something
with atomics and want to record the synchronisation moment for the viewer.

## Save and replay

The viewer's **File → Save** writes the trace to a `.tracy` file. Open it
later with **File → Open** to re-explore without the source process running.
Trace files are usually 50–500 MB for a few minutes of recording — fine
for offline analysis, plan disk space accordingly.

Production tip: when reproducing a customer-reported slow tick, capture a
trace, save, and ship the `.tracy` to the engineer reproducing it.
Everything they need is in the file — no need to set up the same cluster.

## Cost of having Tracy on

| Scenario | Per-zone overhead | Notes |
|---|---|---|
| `release` preset (profiler off) | 0 ns | Macros expand to no-ops; Tracy DLL not linked. |
| Profiler on, no viewer connected (`TRACY_ON_DEMAND`) | ~1 ns | Atomic load + branch. Inert. |
| Profiler on, viewer connected | ~2.25 ns | RDTSC + queue push. Tracy's documented number. |
| Heap alloc with profiler on | ~5–10 ns extra | Re-entry guard (3× TLS) + Tracy hook. |

For a 30 Hz CellApp at 100v100 with ~5k zones per tick, the worst case is
~1.5 ms / s overhead — under 0.5% of CPU. Inert mode is unnoticeable.

## Common pitfalls

- **No frames in the viewer**: process running but no `FrameMark` ever
  fires. Check `ServerConfig::frame_name` is non-empty (it's auto-derived
  from `process_name` if blank).
- **Zones appear but timeline is "stuck"**: the process hasn't ticked yet
  (still in init). Wait a few seconds.
- **Memory tab shows churn far above what you allocate**: STL containers'
  internal allocations are counted too — `std::vector::push_back` that
  triggers a grow is a real `operator new`. Filter by allocator name to
  separate intentional pool churn from incidental STL traffic.
- **Lock zone never appears for a wrapped mutex**: the lock isn't actually
  contended — Tracy only reports waits, not zero-contention acquisitions.
  Force contention from another thread to verify the wrap.
- **C++ and C# zones don't interleave (managed missing)**: backend
  installation failed. Look for the "already installed" warning at boot.

## Where to dig deeper

- Tracy's own manual: <https://github.com/wolfpld/tracy/releases> →
  `tracy.pdf` next to each release. Comprehensive — every macro, every
  viewer feature.
- Atlas's profiler design: [`docs/optimization/profiler_tracy_integration.md`](../optimization/profiler_tracy_integration.md)
- Atlas's macro surface: [`src/lib/foundation/profiler.h`](../../src/lib/foundation/profiler.h)
- Atlas's managed facade: [`src/csharp/Atlas.Shared/Diagnostics/Profiler.cs`](../../src/csharp/Atlas.Shared/Diagnostics/Profiler.cs)
- Tracy 0.13 protocol on the wire: source-only — read
  `build/<preset>/_deps/tracy-src/server/TracyWorker.cpp` for the
  authoritative version.
