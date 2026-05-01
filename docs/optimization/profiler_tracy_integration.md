# Profiler Infrastructure (Tracy + OpenTelemetry)

**Status:** ✅ Shipped (Tracy native + managed; OpenTelemetry bridge
deferred).
**Subsystem:** `src/lib/foundation/profiler.{h,cc}`, `src/lib/server/`,
`src/lib/network/`, `src/lib/clrscript/`, `src/csharp/Atlas.Runtime/`,
`src/csharp/Atlas.Client.Unity/`.

Atlas exposes a single profiling abstraction (`ATLAS_PROFILE_*` /
`Profiler.*`) over Tracy 0.13.x for native code and a self-written
P/Invoke layer for managed code. Both ends share a Tracy listener and
appear interleaved on a unified timeline. The Unity client routes the
same managed surface to `UnityEngine.Profiling.ProfilerMarker` so
Atlas client traces land in the Unity Profiler / Frame Debugger.

OpenTelemetry cross-process span correlation is designed but not
shipped — Tracy's per-process view has been sufficient for current
attribution work. Re-evaluate if multi-process root-cause analysis
becomes a recurring need.

## Hard rules

1. **No file outside `foundation/profiler.{h,cc}` may include
   `<tracy/Tracy.hpp>`.** Code review rejects direct Tracy references;
   `git grep -n 'tracy/Tracy.hpp' src/ | grep -v 'foundation/profiler'`
   must return empty in CI.
2. The C# face is identical on server and client; only the backend
   differs. `Atlas.Client` (netstandard2.1) has zero `UnityEngine`
   reference — Unity-only code lives in `Atlas.Client.Unity`.
3. With `ATLAS_PROFILE_ENABLED=0` (server) or `NullProfilerBackend`
   (client), every `Profile*` / `Profiler.*` call site must compile to
   a no-op the linker can strip.

## Macro / API surface

```cpp
// src/lib/foundation/profiler.h — the only blessed entry point.

ATLAS_PROFILE_FRAME(name)         // FrameMarkNamed; AdvanceTime() tail
ATLAS_PROFILE_FRAME_START(name)   // paired Start / End
ATLAS_PROFILE_FRAME_END(name)

ATLAS_PROFILE_ZONE()              // RAII; auto-name from function
ATLAS_PROFILE_ZONE_N(name)        // explicit literal
ATLAS_PROFILE_ZONE_C(name, color) // RGB-coloured zone
ATLAS_PROFILE_ZONE_TEXT(buf, len) // dynamic text on current zone

ATLAS_PROFILE_PLOT(name, value)   // scalar time series

ATLAS_PROFILE_MESSAGE(buf, len)
ATLAS_PROFILE_MESSAGE_C(buf, len, color)

ATLAS_PROFILE_ALLOC(ptr, size)        // foundation/heap.cc only
ATLAS_PROFILE_FREE(ptr)
ATLAS_PROFILE_ALLOC_NAMED(ptr, size, pool_name)

ATLAS_PROFILE_LOCKABLE(type, var)
ATLAS_PROFILE_LOCK_MARK(var)

#ifndef ATLAS_PROFILE_ENABLED
#  define ATLAS_PROFILE_ENABLED 1
#endif
```

C# mirror in `Atlas.Shared.Diagnostics.Profiler`:

```csharp
using var _ = Profiler.Zone();              // [CallerMemberName]
using var _ = Profiler.Zone("CombatTick");
Profiler.Plot("ActiveBuffs", count);
Profiler.Message($"trace={traceId}");
Profiler.FrameMark("DungeonTick");
```

## Backend wiring

```text
Atlas.Shared boots with NullProfilerBackend.
  ├─ Atlas.Runtime         → installs TracyProfilerBackend
  ├─ Atlas.Client.Unity    → installs UnityProfilerBackend (Unity Awake)
  └─ Atlas.Client.Desktop  → keeps NullProfilerBackend
```

Two backends cannot coexist — `Profiler.Install` rejects the second
call. Unity backend caches `ProfilerMarker` per literal name in a
thread-static dictionary; per-call cost is one dictionary lookup +
`marker.Begin()`. `Plot` routes to `ProfilerCounterValue<T>` on Unity
2022.2+ and falls back to `Profiler.EmitFrameMetaData` on older
versions.

## Build modes

- `debug` / `hybrid` — `ATLAS_ENABLE_PROFILER=ON`.
- `profile` — `RelWithDebInfo` + profiler ON; bundles
  `tracy-profiler` viewer + CLI helpers under `bin/profile/`.
- `release` — `ATLAS_ENABLE_PROFILER=OFF`; profiler symbols absent
  from the binary (verify with `nm` / `dumpbin` after a build-system
  change).

CMake options:

```cmake
option(ATLAS_ENABLE_PROFILER  "Enable Tracy profiler instrumentation" ON)
option(ATLAS_PROFILER_ON_DEMAND "Tracy ON_DEMAND mode" ON)
set(ATLAS_HEAP_ALLOCATOR "mimalloc" CACHE STRING "Heap backend (std | mimalloc)")
```

## Performance budget

| Path | Budget | Notes |
|---|---|---|
| `ATLAS_PROFILE_ZONE`, client connected | ≤ 5 ns | Tracy ~2.25 ns + macro overhead |
| `ATLAS_PROFILE_ZONE`, client disconnected (`ON_DEMAND`) | ≤ 1 ns | atomic load + branch |
| `ATLAS_PROFILE_ZONE`, compile-time disabled | 0 ns | macro empty |
| Total profiler overhead, 100v100 (~5k zone/tick) | ≤ 0.5 ms | derived from above |

A capture that spends > 0.5 ms/tick in profiler bookkeeping fails the
budget — drop zone density in the offending subsystem or downgrade
fine-grained zones to plot counters.

## Caveats

- **Tracy wire-protocol drift** — native and managed must match. The
  managed P/Invoke (`Atlas.Runtime/Diagnostics/TracyNative.cs`) is
  pinned to the same Tracy tag as the C++ submodule; CI must fail on
  mismatch.
- **Sampling stack gaps** — `[UnmanagedCallersOnly]` thunks have no
  PDB, so sampling profilers may show `[unknown]` between native and
  managed zones. Atlas's primary mode is **explicit zones**; sampling
  is secondary diagnostics, never ground truth.
- **Trace-context leakage** — when OpenTelemetry lands, network
  envelope `traceparent` must be cleared at session boundaries
  (login handoff, channel rebuild) to keep unrelated requests out of
  the same span tree.
- **Unity domain reload** — `UnityProfilerBackend` subscribes to
  `AssemblyReloadEvents.beforeAssemblyReload` to flush its marker
  cache; stale marker IDs across a play-session crash IL2CPP.
- **Production exposure** — Tracy's listener port reveals hot-path
  code structure to anyone who can connect. machined must keep the
  port range bound to the cluster-internal interface.

## Operations

- [`docs/operations/profiling.md`](../operations/profiling.md) —
  attaching Tracy GUI, `dotnet-counters`, reading `TickWorkMs`.
- [`docs/operations/tracy_usage.md`](../operations/tracy_usage.md) —
  Tracy GUI walkthrough.

## Rollback path

The macro surface is the contract. Replacing Tracy with another
backend (Optick, Perfetto, internal tooling) only changes
`foundation/profiler.{h,cc}` and the managed `*ProfilerBackend`;
nothing else in the codebase moves. This is why the abstraction is
non-negotiable.
