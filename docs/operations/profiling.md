# Profiling Atlas

This is the operator runbook for capturing performance traces of a running
Atlas server cluster (and, where applicable, the Unity client). For the
design rationale behind the layout below, see
[`docs/optimization/profiler_tracy_integration.md`](../optimization/profiler_tracy_integration.md).
For the practical "how do I use Tracy" walkthrough — viewer install, frame
view, plots, memory tab, lock contention — see [`tracy_usage.md`](tracy_usage.md).

## Build presets

| Preset | Profiler instrumentation | Use when |
|---|---|---|
| `debug` | ON (default) | day-to-day development |
| `release` | OFF | shipping a player-facing binary; zero hidden cost |
| `profile-release` | ON, RelWithDebInfo | production-shaped perf testing — optimised codegen + Tracy zones + symbol info |
| `hybrid` | ON (RelWithDebInfo) | quick perf passes that don't need release-level codegen |

```bash
# Production-shaped perf trace
cmake --preset profile-release
cmake --build build/profile-release --config RelWithDebInfo
```

The `release` preset builds with `ATLAS_ENABLE_PROFILER=OFF`, which compiles
every `ATLAS_PROFILE_*` macro to a no-op at the preprocessor stage and drops
the Tracy DLL link entirely. There is no runtime toggle that can re-enable
profiling on a `release` binary — that is intentional, and matches the
"shipping binary has no profiler" promise documented in the integration doc.

## Capturing a server trace (Tracy)

Atlas links Tracy as a SHARED library named `TracyClient.dll` (Windows) or
`libTracyClient.so` (Linux). The client side runs in-process, listens for
viewer connections on a TCP port, and stays inert until a viewer attaches
(`TRACY_ON_DEMAND` is on by default — see `cmake/Dependencies.cmake`).

### Single process

1. Start the server normally (e.g. `bin/profile-release/server/atlas_cellapp.exe`).
2. Launch the Tracy viewer.
3. Click **Connect**, leave the address as `127.0.0.1`, port `8086`.

The first frame appears as soon as the viewer attaches; nothing earlier is
buffered, by design — `ON_DEMAND` mode means zero cost when no one is looking.

### Multiple processes on the same host

Tracy auto-falls-back the listen port: the first process gets `8086`, the
second `8087`, and so on. The viewer's **Discover** scan walks that range
and lists every active session by program name. machined does not need to
inject `TRACY_PORT` for this to work.

If two processes happen to grab adjacent ports and you want them on
specific numbers (e.g. CellApp 0 always at 9000), pass the port at process
spawn through Tracy's compile-time `TRACY_DATA_PORT` define and a custom
build — the runtime override path is not currently wired in Atlas. See
"Future work" below.

### Reading the trace

The default view shows the timeline, one row per thread. Frame markers
(`OpenWorldTick`, `<process_name>.Tick`, configurable via
`ServerConfig::frame_name`) split the timeline into logical ticks. Useful
plots:

| Plot | Source | What it tells you |
|---|---|---|
| `TickWorkMs` | `ServerApp::AdvanceTime` | per-tick work time. Spikes correlate with slow-tick log warnings. |
| `BytesOut` | `Channel::Send` | per-packet outbound size. Large values often correlate with `Witness::Update::Pump`. |
| `BytesIn` | `Channel::OnDataReceived` | per-packet inbound size. |

Per-pool memory streams (added in Phase 6 of the integration plan) appear
in the **Memory** tab — `TimerNode` is one such stream; future pools added
under `PoolAllocator(name, …)` show up here automatically.

### Cluster-wide trace

A 4-process cluster spawned by machined (`atlas_cellapp` × 2, `atlas_baseapp`,
`atlas_loginapp`) ends up with four Tracy listeners on adjacent ports. The
viewer can attach to one at a time; switching between them keeps the timeline
state clean. There is no single-window cluster view today — that is the OTel
distributed-trace work intentionally deferred from Phase 5b of the
integration plan.

## Capturing a client trace (Unity Profiler)

The Atlas Unity client routes the same zone names through `ProfilerMarker`,
visible in the Unity Profiler window:

1. Build the Unity client; ensure `Atlas.Client.Unity.dll` is present.
2. Bootstrap the backend during application start:
   ```csharp
   Atlas.Diagnostics.Profiler.SetBackend(new Atlas.Client.Unity.UnityProfilerBackend());
   ```
3. Open **Window → Analysis → Profiler**. Connect to the running player
   (Editor or device).

Zone names match the server's via `Atlas.Diagnostics.ProfilerNames` — for
example `ClientCallbacks.DispatchPropertyUpdate` on the client lines up with
the server's `Channel::Send` zone for the same logical property delta when
both traces are timestamped. See `Atlas.Client.Unity/README.md` for the
domain-reload caveat.

## C# heap and GC

Tracy's memory hooks cover native (C++) allocations only. Managed
allocations on the server go through `dotnet-counters` and `dotnet-gcdump`:

```bash
# Live counters — GC pressure, allocation rate, server tick
dotnet-counters monitor --process-id <PID> System.Runtime Atlas.*

# One-shot heap snapshot for leak hunting
dotnet-gcdump collect --process-id <PID> --output cellapp.gcdump
```

Open `.gcdump` in Visual Studio or PerfView for object-graph analysis.

## Allocator switching

The default heap is `std` (platform CRT). For perf comparison work, swap
to mimalloc:

```bash
cmake -B build/profile-release-mimalloc \
      --preset profile-release \
      -DATLAS_HEAP_ALLOCATOR=mimalloc
```

Two configurations targeting the same `RelWithDebInfo` build coexist
without overwriting each other's `bin/` outputs — the build directory
name becomes the bin folder name (see patch 0009). Run both, compare
Tracy traces side by side.

## Diagnosing missing zones

Symptoms and likely causes:

| Symptom | Likely cause |
|---|---|
| Tracy viewer shows zero frames | `release` preset (profiler off), or the process hasn't ticked yet |
| C# zones missing, C++ zones present | `Profiler.SetBackend(new TracyProfilerBackend())` not yet called by `Lifecycle.DoEngineInit` |
| Plot stays at 0 | The plot value is reported only when its callsite executes; tick-driver plots like `TickWorkMs` need the work bracket to run at least once |
| `Witness::Update::Pump` is empty | No witness peers — load real entities via stress harness |

## Future work (not in current phases)

- **Cross-process span correlation (OTel)**: Phase 5b in the integration
  plan; not started. The wire-format envelope change required would touch
  every `bundle.cc` consumer.
- **Deterministic Tracy ports per process**: today's auto-fallback is
  fine for development. Production deployments that want stable ports per
  CellApp instance would route through a Tracy compile-time recompile or
  a Tracy 0.13+ runtime port API once we wire it.
- **Generator-emitted property apply zones**: the Atlas C# def generator
  could emit `Profiler.Zone(...)` in each generated `ApplyReplicatedDelta`
  override. Not in scope for the profiler integration phases — belongs
  with the next codegen pass.
