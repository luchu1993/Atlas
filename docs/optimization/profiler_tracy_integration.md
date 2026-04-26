# Profiler Infrastructure (Tracy + OpenTelemetry)

**Priority:** P0 (prerequisite — all other optimization tasks depend on this for measurement)
**Subsystem:** `src/lib/foundation/`, `src/lib/server/`, `src/lib/network/`, `src/lib/clrscript/`, `src/csharp/Atlas.Runtime/`
**Impact:** Establishes the measurement substrate. Until this lands, every other doc in this directory is conjecture — we cannot quantify "before vs after" without a unified C++/C# timeline, frame markers, and zone-level breakdowns of the 100v100 hot path.

## Status (April 2026)

| Phase | Patch | Status | Notes |
|---|---|---|---|
| 1 — Dependency wiring + macro shell | 0001 | ✅ landed | Tracy fetch (later bumped to 0.13.1 in patch 0004); `foundation/profiler.h` macro surface |
| 2 — Frame markers + tick driver | 0002 | ✅ landed | `ServerApp::AdvanceTime` zone + `TickWorkMs` plot + per-process frame name |
| 3 — Hot-path zones for 100v100 | 0003 | ✅ landed | 24 zones across cellapp / witness / space / cell_entity / real_entity_data |
| 4 — Tracy-CSharp integration | 0004, 0005 | ✅ landed (deviated) | Tracy bumped to 0.13.1, built SHARED. Self-written P/Invoke (LibraryImport) instead of Tracy-NET — that NuGet requires net10 and Atlas.Runtime is net9. Single TracyClient.dll backs both C++ and managed sides. |
| 5a — Network zones + byte plots | 0006 | ✅ landed | `Channel::Send` / `Dispatch` zones + `BytesIn`/`BytesOut` plots + per-message id text |
| 5b — OpenTelemetry cross-process | — | ❌ **deferred** | Wire-format envelope change + OTel SDK weight not justified by current 100v100 attribution needs. Tracy's in-process view covers what we need. |
| 6 — Memory hooks + per-pool tracking | 0007 | ✅ landed | `atlas::Heap` abstraction + global `operator new`/`delete` override (20 variants) + named Tracy hooks per pool (`PoolAllocator(name, …)`) |
| 6+ — mimalloc backend selectable | 0008 | ✅ landed | `ATLAS_HEAP_ALLOCATOR=std\|mimalloc` CMake string; future allocators (jemalloc, …) plug in with a 3-step extension |
| (output layout) | 0009 | ✅ landed | `bin/<build_dir>/...` so parallel CMake build dirs (e.g. `build/debug` + `build/profile-release-mimalloc`) don't overwrite each other's artifacts |
| 7 — Client zones + Unity backend | 0010 | ✅ landed (deviated) | `ClientCallbacks` + `ClientEntity.ApplyPositionUpdate` zones; `Atlas.Client.Unity` (asmdef + `UnityProfilerBackend`) outside Atlas's dotnet pipeline (Unity-only). `Atlas.Client.Desktop` keeps `NullProfilerBackend` rather than duplicating Tracy bindings. |
| 8 — Build modes + runbook | 0011 | ✅ landed | `release` preset turns profiler OFF; new `profile-release` (RelWithDebInfo + profiler ON); operator runbook at [`docs/operations/profiling.md`](../operations/profiling.md). machined env-var TRACY_PORT injection skipped — Tracy auto-falls-back ports and the viewer's Discover scan handles multi-process attach. |

The integration is **functionally complete** for the 100v100 attribution
goal. The two intentional deferrals (5b OTel, 7 Atlas.Client.Desktop Tracy)
are noted in the relevant phase descriptions below and have explicit
"future work" entries in the operator runbook.

## Background

Atlas runs C++ tick loops (20 Hz open world / 30 Hz dungeon) that call into C# combat
logic via CoreCLR + `[UnmanagedCallersOnly]`. Optimization work needs to attribute cost
between native tick stages, managed script callbacks, network serialization, and
cross-process RPC fan-out. No existing instrumentation provides this view.

## Goals

1. Single timeline that interleaves native zones with managed zones, sharing one clock
   source so a frame trace can show `CellApp::Tick → ScriptHost::Update → C#
   CombatLogic.OnTick` contiguously.
2. Frame markers per process aligned with `ServerApp::AdvanceTime()` — distinct
   labels for `OpenWorldTick` (20 Hz) and `DungeonTick` (30 Hz).
3. Zone overhead under ~5 ns on the hot path; no measurable cost when the profiler
   client is disconnected.
4. Live, attach-on-demand viewing of any production server process; no stop-the-world
   restart required to start profiling.
5. Cross-process causality: a network message can be followed from sender process to
   receiver process in a single trace UI.
6. **Mandatory abstraction layer**: every site of instrumentation in Atlas code uses
   Atlas-defined macros (`ATLAS_PROFILE_*`) — never raw Tracy/OTel symbols. This is
   non-negotiable. Swapping Tracy for another backend (Optick, Perfetto, internal
   tool) must be a single-file change inside `foundation/profiler.h`.
7. **Client SDK target is Unity**: the same logical surface (`Profiler.Zone`,
   `Profiler.FrameMark`, `Profiler.Plot`) compiles for `Atlas.Client` (netstandard2.1)
   and routes, on a Unity build, to `UnityEngine.Profiling.ProfilerMarker` /
   `Profiler.BeginSample` so traces land natively in the Unity Profiler window /
   Unity Profile Analyzer / Frame Debugger. Unity-side instrumentation must never
   pay the cost of Tracy and must be inlinable down to a `ProfilerMarker.Auto()` so
   IL2CPP can fold it into the host engine's existing sampler infrastructure.

## Non-Goals

- GPU profiling (server has none).
- Statistical sampling profiles for capacity planning — those go through `perf` /
  ETW separately, outside this framework.
- Replacing `MemoryTracker` — Tracy's allocator hooks supplement it, do not subsume.
- Per-allocation tagging in C# heap — `dotnet-gcdump` runs on demand, not via this
  layer.

## Solution Overview

| Concern | Tool | Where |
|--------|------|------|
| Native zones, frames, plots, locks | Tracy 0.11.x | C++ everywhere |
| Managed zones (server), sharing Tracy clock | Tracy-CSharp (NuGet) | `Atlas.Runtime` and downstream user scripts |
| Managed zones (client) | Unity `ProfilerMarker` / `Profiler.BeginSample` | `Atlas.Client` consumed inside Unity |
| Cross-process span linking | OpenTelemetry C++ + .NET SDK | `network/channel.cc`, `Atlas.Runtime/Hosting` |
| C# heap deep-dives (on demand) | `dotnet-gcdump`, `dotnet-counters` (server); Unity Memory Profiler (client) | external, attach via PID |
| Native heap deep-dives | `TracyAlloc` / `TracyFree` wrapped by `MemoryTracker` | `foundation/memory_tracker.cc`, `pool_allocator.cc` |

Tracy carries the inner-loop, nanosecond-resolution view. OpenTelemetry carries the
inter-process, millisecond-resolution view. The two are bridged by the trace ID:
each network message header gains a W3C `traceparent`, which Atlas writes into Tracy
as a zone text annotation so the UI can cross-reference.

## Macro API (the only surface code may touch)

`src/lib/foundation/profiler.h` defines the entire abstraction. **No file outside
`foundation/profiler.cc` may include `<tracy/Tracy.hpp>`.** Code review must reject
direct Tracy references.

```cpp
// Frame markers — one per top-level tick driver call site.
ATLAS_PROFILE_FRAME(name)         // FrameMarkNamed; at end of AdvanceTime()
ATLAS_PROFILE_FRAME_START(name)   // for paired Start/End forms
ATLAS_PROFILE_FRAME_END(name)

// Scoped zones — RAII, costs ~2 ns when client connected, 0 when not.
ATLAS_PROFILE_ZONE()              // function name auto-captured
ATLAS_PROFILE_ZONE_N(name)        // explicit name, compile-time literal
ATLAS_PROFILE_ZONE_C(name, color) // colored zone (RGB hex)
ATLAS_PROFILE_ZONE_TEXT(buf, len) // attach dynamic string to current zone

// Plots — scalar time series (entity counts, queue depths, bandwidth).
ATLAS_PROFILE_PLOT(name, value)

// Free-form messages and trace-id annotations.
ATLAS_PROFILE_MESSAGE(buf, len)
ATLAS_PROFILE_MESSAGE_C(buf, len, color)

// Allocator hooks — used inside MemoryTracker / pool_allocator only.
ATLAS_PROFILE_ALLOC(ptr, size)
ATLAS_PROFILE_FREE(ptr)
ATLAS_PROFILE_ALLOC_NAMED(ptr, size, pool_name)

// Lock contention — wrap mutex types.
ATLAS_PROFILE_LOCKABLE(type, var)
ATLAS_PROFILE_LOCK_MARK(var)

// Compile-time switch — when undefined, every macro expands to a no-op.
#ifndef ATLAS_PROFILE_ENABLED
#  define ATLAS_PROFILE_ENABLED 1
#endif
```

C# side mirrors the same surface. **The C# surface is identical across server and
client; only the backend differs.**

```csharp
using var _ = Profiler.Zone();              // [CallerMemberName] auto name
using var _ = Profiler.Zone("CombatTick");
Profiler.Plot("ActiveBuffs", count);
Profiler.Message($"trace={traceId}");
Profiler.FrameMark("DungeonTick");
```

The surface is split across two assemblies to keep Unity-incompatible code out of
`Atlas.Client`:

- `Atlas.Shared/Diagnostics/Profiler.cs` — the public, surface-only API.
  netstandard2.1 friendly. Calls a swappable `IProfilerBackend` interface. Default
  backend is `NullProfilerBackend` (every method a no-op JIT inlines away).
- `Atlas.Runtime/Diagnostics/TracyProfilerBackend.cs` — server backend, wires to
  Tracy-CSharp. Installed by `ScriptHost` at boot.
- `Atlas.Client.Unity/Diagnostics/UnityProfilerBackend.cs` (new assembly, Unity-only,
  conditionally compiled with `#if UNITY_2022_3_OR_NEWER`) — installs a backend that
  `[MethodImpl(AggressiveInlining)]`-routes `Zone()` to a cached `ProfilerMarker.Auto()`
  and `FrameMark` to `UnityEngine.Profiling.Profiler.EndSample/BeginSample` paired
  with the Unity frame.

The Unity backend must:
1. Not introduce any non-Unity dependency in `Atlas.Client` itself — `Atlas.Client`
   stays at netstandard2.1 with zero references to UnityEngine.
2. Use `ProfilerMarker` (preferred over `Profiler.BeginSample` — IL2CPP folds it
   to a single `BurstStart/End` call when available).
3. Cache markers by name in a thread-static dictionary so the per-call cost is one
   dictionary lookup + one `marker.Begin()`. Static-readonly markers are preferred
   wherever the call site can be expressed as a `Zone("LiteralName")`.
4. Forward `Plot(name, value)` to `Profiler.EmitFrameMetaData` or to a custom
   counter via `ProfilerCategory.Scripts` (Unity 2022.2+).
5. Never load Tracy native libraries; the Unity build must not even reference
   Tracy-CSharp.

Both the C++ and C# macro layers must compile to no-ops under
`ATLAS_PROFILE_ENABLED=0` (server) and when no backend is installed (client).
Specifically, on the Unity client, shipping a release build with the
`UnityProfilerBackend` excluded from the build profile must reduce every
`Profiler.Zone` call site to dead code that the IL2CPP linker strips.

## Phased Implementation

### Phase 1 — Dependency wiring and macro shell

**Files:**
- `cmake/Dependencies.cmake` — add Tracy `FetchContent` (pinned tag `v0.11.1`) and
  the OpenTelemetry C++ SDK (header-only metrics + tracing portion only).
- `cmake/AtlasCompilerOptions.cmake` — define `ATLAS_PROFILE_ENABLED` from the new
  CMake option `ATLAS_ENABLE_PROFILER` (default ON for `debug` and `hybrid`, OFF for
  `release` until Phase 7 flips it).
- `src/lib/foundation/profiler.h` — full macro surface, both enabled and disabled
  branches. Tracy headers included **only inside this file** (and `profiler.cc`).
- `src/lib/foundation/profiler.cc` — small implementation file for macros that
  cannot be header-only (e.g. plot config, broadcast disable).
- `src/lib/foundation/CMakeLists.txt` — add sources, link `TracyClient`.
- `tests/unit/foundation/test_profiler.cc` — verify all macros compile under both
  `ATLAS_PROFILE_ENABLED=0` and `=1`, and that zero-mode produces no symbol
  references to Tracy.

**Acceptance:**
- `cmake --preset debug && cmake --build build/debug` succeeds with profiler ON.
- `cmake --preset release` succeeds with profiler OFF (no Tracy symbols in binary —
  verified via `nm` / `dumpbin`).
- New unit test passes under both modes.

### Phase 2 — Frame markers and the tick driver

**Files:**
- `src/lib/server/server_app.cc:206` `ServerApp::AdvanceTime()` — wrap the
  work-bracketed section with `ATLAS_PROFILE_ZONE_N("Tick")` and emit
  `ATLAS_PROFILE_FRAME(config_.frame_name)` at the end. `frame_name` comes from
  `ServerAppConfig` (new field, default `"Tick"`; CellApp/BaseApp set
  `"OpenWorldTick"` / `"DungeonTick"` based on Space type).
- `src/lib/server/entity_app.cc` — zones around entity event drain and updatable
  groups, named per group (`Updatables_L0`, `Updatables_L1`).
- `src/lib/server/server_app.h` — extend `TickStats` plot of `last_work_duration`
  via `ATLAS_PROFILE_PLOT("TickWorkMs", ms)` so frame-time history is visible
  alongside zones.

**Acceptance:**
- Tracy GUI connecting to a running CellApp shows continuous frame bars labeled
  per Space type.
- Slow-tick warnings in logs correlate to spikes visible on the `TickWorkMs` plot.

### Phase 3 — Hot-path zones in the 100v100 critical sections

**Files:**
- `src/server/cellapp/cellapp.cc` — zones around the witness drain loop, AoI rebuild,
  controller resolve.
- `src/server/cellapp/witness.cc` — zones for `Update()`, priority heap rebuild,
  delta serialization, send loop.
- `src/server/cellapp/space.cc` — zones for spatial-query callbacks.
- `src/server/cellapp/cell_entity.cc` — zones for `OnRealEntityUpdate` and ghost
  fan-out.
- `src/server/cellapp/real_entity_data.cc` — zone for delta envelope build.

**Naming convention:** `Subsystem::Method` — e.g. `"Witness::Update"`,
`"RealEntityData::BuildDelta"`. This matches Tracy's source-location capture and
keeps the GUI legible.

**Acceptance:**
- A 100v100 stress trace shows ≥30 named zones per tick, accounting for ≥90% of
  the measured work duration. Any "missing time" gap above 5% must be filled with
  an additional zone before declaring the phase done.

### Phase 4 — Tracy-CSharp integration

**Files:**
- `src/csharp/Atlas.Runtime/Atlas.Runtime.csproj` — add `Tracy.CSharp` package
  reference (pinned to a release whose wire protocol matches the chosen native
  Tracy tag).
- `src/csharp/Atlas.Runtime/Diagnostics/Profiler.cs` — new file mirroring the
  macro surface from C++ (`Zone`, `FrameMark`, `Plot`, `Message`).
- `src/csharp/Atlas.Runtime/Hosting/ScriptHost.cs` — bring up the managed Tracy
  client during boot, **after** native `ClrHost::Initialize` returns. Both sides
  must publish to the same Tracy listener port.
- `src/lib/clrscript/clr_host.cc` — surface the Tracy server port via the existing
  config plumbing so the managed side picks it up via env var
  (`TRACY_PORT`, default 9000+pid).
- `src/csharp/Atlas.Runtime/Entity/*` — instrument entity tick callbacks and
  property-setter hot paths discovered in Phase 3.

**Acceptance:**
- A single Tracy GUI session shows interleaved native and managed zones with
  contiguous timestamps.
- The managed `Profiler.Zone("CombatLogic.OnTick")` appears nested under the
  native `ScriptHost::InvokeTick` zone — no time gap.

### Phase 5 — Network message tracing and OpenTelemetry bridge

**Files:**
- `src/lib/network/bundle.h` / `bundle.cc` — extend message envelope with optional
  `traceparent` (16-byte trace_id + 8-byte span_id + flags). Wire bump documented
  in this phase, gated on the existing `kAtlasAbiVersion` bump.
- `src/lib/network/channel.cc:41` `Channel::Send()` — emit
  `ATLAS_PROFILE_ZONE_N("Channel::Send")` and `ATLAS_PROFILE_PLOT("BytesOut", n)`;
  if an OTel context is active, serialize it into the new envelope field; emit
  `ATLAS_PROFILE_MESSAGE("trace=<id>")`.
- `src/lib/network/channel.cc:189` dispatch — open a Tracy zone named after the
  message ID's symbolic form, restore OTel context from the envelope.
- `src/lib/network/CMakeLists.txt` — link OTel SDK.
- `src/csharp/Atlas.Runtime/Hosting/ScriptHost.cs` — initialize the .NET
  `OpenTelemetry` provider with the same service name that machined registers.
- New `cmake/AtlasOtelDeploy.cmake` — optional Jaeger/Tempo collector endpoint,
  configured via `atlas.otel.endpoint` in `server_config`.

**Acceptance:**
- Sending a message from BaseApp to CellApp produces two Tracy zones (one per
  process) carrying matching trace IDs in their text annotations.
- The same trace ID appears as a Jaeger span tree.
- When `atlas.otel.endpoint` is unset, no OTel network traffic occurs (verified
  by counter on the OTel exporter being zero).

### Phase 6 — Memory and pool allocator hooks

**Files:**
- `src/lib/foundation/memory_tracker.cc` — wrap `RecordAlloc` / `RecordDealloc`
  with `ATLAS_PROFILE_ALLOC` / `ATLAS_PROFILE_FREE`.
- `src/lib/foundation/pool_allocator.cc` — annotate per-pool acquire/release with
  `ATLAS_PROFILE_ALLOC_NAMED(ptr, size, pool_name_)`. Pool name comes from the
  existing pool registration metadata.
- `src/lib/foundation/intrusive_ptr.h` — leave untouched; Tracy already attributes
  through the underlying allocator.

**Acceptance:**
- Tracy "Memory" tab shows separate streams per pool name.
- A controlled leak (allocate without free) is visible in the unfreed-allocation
  list within 1 second of the leak occurring.

### Phase 7 — Client SDK profiler abstraction (Unity-compatible)

**Files:**
- `src/csharp/Atlas.Shared/Diagnostics/Profiler.cs` — public macro-mirror API
  (`Zone`, `Plot`, `Message`, `FrameMark`). All methods route through
  `IProfilerBackend Profiler.Backend`; default backend is `NullProfilerBackend`.
  Methods are marked `[MethodImpl(MethodImplOptions.AggressiveInlining)]`.
- `src/csharp/Atlas.Shared/Diagnostics/IProfilerBackend.cs` — interface:
  `BeginZone(string)`, `EndZone(IntPtr)`, `Plot(string, double)`,
  `Message(string)`, `FrameMark(string)`. The token returned by `BeginZone` is an
  opaque `IntPtr` so backends can cache markers without the surface caring.
- `src/csharp/Atlas.Shared/Diagnostics/NullProfilerBackend.cs` — no-op fallback.
- `src/csharp/Atlas.Client.Unity/` (new project, Unity-only, **netstandard2.1**,
  shipped as a managed plugin DLL into `Packages/com.atlas.client/Runtime/`) —
  contains `UnityProfilerBackend.cs`:
  - `ProfilerMarker` cache keyed by literal name, populated on first `BeginZone`.
  - For literal-name zones, code-gen analyzer (Phase 7 stretch goal) replaces
    `Profiler.Zone("Name")` call sites with `_marker_Name.Auto()` to bypass the
    dictionary entirely.
  - `Plot` forwards to `ProfilerCounterValue<T>` (Unity 2022.2+). On older Unity
    versions falls back to `Profiler.EmitFrameMetaData`.
  - `FrameMark` is a no-op — Unity drives its own frame markers; the server-side
    frame name is forwarded as a `Profiler.BeginSample` to disambiguate logical
    tick frames from render frames.
- `src/csharp/Atlas.Client.Unity/Atlas.Client.Unity.asmdef` — Unity assembly
  definition with `defineConstraints: ["UNITY_2022_3_OR_NEWER"]` and a
  `versionDefines` entry for the Unity Profiler API.
- `src/csharp/Atlas.Client.Desktop/DesktopBootstrap.cs` — installs
  `TracyProfilerBackend` (the same one Atlas.Runtime uses) so non-Unity sample
  hosts and desktop tests still see traces via Tracy.
- `src/client_sdk/atlas_client_profiler.h` (new, **only if a future C++ client
  plugin is added** — currently the client is pure C# under Unity, so this file
  is a placeholder noted here for completeness and only created when the native
  movement/skill plugin lands).
- `src/csharp/Atlas.Client/ClientHost.cs` — instrument the client tick boundary
  with `Profiler.FrameMark("ClientTick")`; instrument message dispatch.
- `src/csharp/Atlas.Client/ClientEntity.cs` — instrument property apply, predicted
  movement, prediction reconciliation.

**Backend selection (compile/load order):**

```
Atlas.Shared boots with NullProfilerBackend.
  └─ Atlas.Client.Desktop bootstrap → installs TracyProfilerBackend  (desktop hosts)
  └─ Atlas.Client.Unity (Unity Awake)→ installs UnityProfilerBackend (Unity hosts)
```

The application chooses one. Trying to install a second logs a warning and is
ignored. There is no merging — the two backends target different UIs by design.

**Acceptance:**
- A Unity sample scene runs the Atlas client; the Unity Profiler window shows
  named samples corresponding to `ClientHost.Tick`, `ClientEntity.ApplyDelta`,
  predicted movement zones — with the same names that show up in Tracy on the
  desktop sample.
- `Atlas.Client.csproj` (netstandard2.1) builds without referencing UnityEngine.
- IL2CPP build with `Profiler.Backend = NullProfilerBackend` strips zone bodies
  (verified by inspecting the generated C++ for one known site).
- Logical zone names match between server (Tracy) and client (Unity Profiler) for
  bidirectional features (e.g. `Combat.OnDamage` shows up on both sides linked by
  `traceparent` in messages — see Phase 5).

### Phase 8 — Build modes, deployment, and machined orchestration

**Files:**
- `CMakePresets.json` — add `profile-release` preset (RelWithDebInfo +
  `ATLAS_ENABLE_PROFILER=ON`); leave plain `release` profiler-off.
- `src/server/machined/` — at process spawn, set `TRACY_PORT=9000+local_pid_slot`
  and `OTEL_SERVICE_NAME=<process_type>-<instance_id>` env vars so that running
  multiple CellApps on one host does not collide.
- `docs/operations/profiling.md` (new, brief operator runbook) — how to attach
  Tracy GUI, how to attach `dotnet-counters`, how to read `TickWorkMs` plot.

**Acceptance:**
- Spawning a 4-process cluster via machined assigns four distinct Tracy ports.
- A single Tracy GUI can iterate across the four processes without restart.

## CMake Option Surface

```cmake
option(ATLAS_ENABLE_PROFILER  "Enable Tracy/OTel profiler instrumentation" ON)
option(ATLAS_PROFILER_ON_DEMAND "Tracy ON_DEMAND mode (zero cost when no client)" ON)
option(ATLAS_PROFILER_QUEUE_MB "Tracy event queue size, megabytes" 256)
```

`ATLAS_PROFILER_QUEUE_MB` defaults to 256 (vs Tracy's 64 stock) because a 100v100
tick can emit ≥10k zones; the stock buffer empirically saturates under load.

## Performance Budget

| Operation | Budget | Source |
|---|---|---|
| `ATLAS_PROFILE_ZONE` enabled, client connected | ≤ 5 ns | Tracy manual (~2.25 ns) + macro overhead |
| `ATLAS_PROFILE_ZONE` enabled, client disconnected (`ON_DEMAND`) | ≤ 1 ns | branch + atomic load |
| `ATLAS_PROFILE_ZONE` disabled at compile time | 0 ns | macro empty |
| OTel span open/close per network message | ≤ 200 ns | OTel C++ SDK micro-benchmark |
| Per-tick total profiler overhead, 100v100, ~5k zones | ≤ 0.5 ms | derived from above |

If a phase's stress test shows >0.5 ms per-tick attributable to profiler overhead,
that phase fails acceptance — reduce zone density or move to lower-rate sampling
in that subsystem.

## Risks

- **Wire protocol drift**: Tracy native and Tracy-CSharp must speak the same
  protocol version. Pin both to a known-good pair (Phase 1 + Phase 4) and refuse
  to bump one without the other. CI build must fail if the C# package version and
  the C++ submodule tag are inconsistent.
- **Managed call-stack gaps under sampling**: `[UnmanagedCallersOnly]` thunks have
  no PDB; the sampling profiler may show "[unknown]" frames between native and
  managed zones. Mitigation: the abstraction's primary mode is **explicit zones,
  not sampling**. Sampling is a secondary diagnostic, never the source of truth.
- **Queue saturation in 100v100**: see Phase 1 default of 256 MB queue. If still
  saturating, downgrade fine-grained zones in the witness inner loop to plot
  counters instead of per-event zones.
- **Trace context leaking across logical sessions**: `traceparent` in network
  envelope must be cleared at session boundaries (login handoff, channel
  re-establishment) to avoid joining unrelated request trees. Audit during
  Phase 5.
- **Profiler-on production**: even with `ON_DEMAND`, an attacker who can reach the
  Tracy port observes hot-path code structure. The Tracy listener must bind to
  the cluster-internal interface only; machined is responsible for not exposing
  the port range externally.
- **Unity backend marker leak**: Unity's `ProfilerMarker` cache must be cleared on
  domain reload (Unity Editor) — surviving managed objects pinned to a stale
  marker ID will crash the next play session. The `UnityProfilerBackend` must
  subscribe to `AssemblyReloadEvents.beforeAssemblyReload` and flush its cache.
- **Server↔client zone name divergence**: features instrumented on both sides
  (e.g. damage application visible in server Tracy and client Unity Profiler)
  must use shared name constants. Define them in `Atlas.Shared/Diagnostics/
  ProfilerNames.cs` so a typo in one side does not break trace correlation.
- **Abstraction erosion**: someone will eventually `#include <tracy/Tracy.hpp>`
  directly to access a Tracy-only feature. Guard via a CI grep:
  `git grep -n 'tracy/Tracy.hpp' src/ | grep -v 'foundation/profiler'` must
  return empty.

## Rollback Plan

If Tracy proves unworkable mid-rollout (e.g. wire-protocol blocker, license
re-evaluation): replace `foundation/profiler.{h,cc}` with a backend pointing at a
substitute (microprofile, Optick, or an internal ring-buffer dumper). The macro
surface is the contract — no other file changes. This is the entire reason
Phase 1 is non-negotiable about the abstraction.

## Key Files (summary)

- `src/lib/foundation/profiler.h` — **the only file allowed to include Tracy headers**
- `src/lib/foundation/profiler.cc`
- `src/lib/foundation/memory_tracker.cc`
- `src/lib/foundation/pool_allocator.cc`
- `src/lib/server/server_app.cc` — frame markers
- `src/lib/server/entity_app.cc`
- `src/lib/network/channel.cc` — send/recv zones, OTel context
- `src/lib/network/bundle.h` / `bundle.cc` — `traceparent` envelope
- `src/lib/clrscript/clr_host.cc` — Tracy port handoff to managed side
- `src/csharp/Atlas.Shared/Diagnostics/Profiler.cs` — public macro mirror (server + client)
- `src/csharp/Atlas.Shared/Diagnostics/IProfilerBackend.cs`
- `src/csharp/Atlas.Shared/Diagnostics/NullProfilerBackend.cs`
- `src/csharp/Atlas.Shared/Diagnostics/ProfilerNames.cs` — shared zone name constants
- `src/csharp/Atlas.Runtime/Diagnostics/TracyProfilerBackend.cs` — server backend
- `src/csharp/Atlas.Runtime/Hosting/ScriptHost.cs` — managed Tracy + OTel bring-up
- `src/csharp/Atlas.Client.Unity/UnityProfilerBackend.cs` — Unity `ProfilerMarker` backend
- `src/csharp/Atlas.Client.Unity/Atlas.Client.Unity.asmdef`
- `src/csharp/Atlas.Client.Desktop/DesktopBootstrap.cs` — installs Tracy backend for desktop
- `src/csharp/Atlas.Client/ClientHost.cs`, `ClientEntity.cs` — client zone instrumentation
- `src/server/cellapp/{cellapp,witness,space,cell_entity,real_entity_data}.cc`
- `cmake/Dependencies.cmake` — Tracy + OTel fetch
- `cmake/AtlasCompilerOptions.cmake` — `ATLAS_PROFILE_ENABLED` plumbing
- `CMakePresets.json` — `profile-release` preset
- `tests/unit/foundation/test_profiler.cc`
- `docs/operations/profiling.md`
