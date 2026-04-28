# Script PublishReplicationAll — GC Pressure & Allocation Audit

**Status:** ✅ Round-1 shipped (boxing eliminated, recurring GC spikes
gone).  Further reductions (batch P/Invoke, dirty-skip) deferred until
business complexity grows.
**Subsystem:** `src/csharp/Atlas.Runtime/Entity/EntityManager.cs`,
`src/csharp/Atlas.Runtime/Core/Lifecycle.cs`,
`src/csharp/Atlas.Generators.Def/Emitters/PropertyCodec.cs`,
`src/csharp/Atlas.Shared/Observable/`
**Impact:** Recurring GC spikes inside `Script.PublishReplicationAll`
removed; managed allocation rate on the tick hot path dropped to a
near-flat baseline.

> Original investigation (priority P1, before any fix) is preserved
> below for context — see **What we shipped** for the actual change set
> and **Remaining work** for what was deliberately left for later.

## Profiling evidence (before)

Original baseline run (100 clients, 120 s):

| Metric | Value |
|---|---|
| `Script.PublishReplicationAll` mean | 307 μs |
| `Script.PublishReplicationAll` p99  | 896 μs |
| `Script.PublishReplicationAll` max  | **6,590 μs** |
| Ticks > 1 ms | 7 / 723 (0.97%) |
| Ticks > 2 ms | 3 / 723 (0.41%) |
| Spike timestamps | clustered at ramp-up (5.7 s, 10.4 s) and 65–68 s |

The max=6.59 ms spike was 7× p99 — a **discrete pause event**, not
algorithmic slowness.  Repeating spikes at 65–68 s ruled out a one-off
ramp-up artefact.  `Script.OnTick` max (7.77 ms) was almost entirely
explained by `PublishReplicationAll`.

## Current behaviour

`EntityManager.PublishReplicationAll()` runs once per tick across all
entities:

```csharp
var ownerSnap  = new SpanWriter(256);   // ArrayPool.Rent on first Reset
var otherSnap  = new SpanWriter(256);
var ownerDelta = new SpanWriter(128);
var otherDelta = new SpanWriter(128);
try {
    foreach (var entity in _entities.Values) {
        ownerSnap.Reset(); otherSnap.Reset();
        ownerDelta.Reset(); otherDelta.Reset();
        entity.BuildAndConsumeReplicationFrame(...);
        NativeApi.PublishReplicationFrame(...);  // P/Invoke per entity
    }
} finally {
    ownerSnap.Dispose(); ...  // ArrayPool.Return
}
```

The `SpanWriter` quartet is shared across entities (zero-alloc by
contract).  The hot allocation surface lived inside the codegen-emitted
`BuildAndConsumeReplicationFrame` — see **What we shipped** for what was
fixed and what's left.

## What we shipped

`dotnet-trace` with `gc-verbose` providers attributed ~14% of a 60 s
trace window (8.5 s wall) to `IEnumerable<T>.GetEnumerator()` interface
dispatches in the codegen path.  Each `foreach` over a property exposed
as `IReadOnlyList<T>` / `IReadOnlyDictionary<K,V>` boxed the otherwise-
struct `List<T>.Enumerator` to the heap once per call, generating per-
tick gen0 pressure that triggered the recurring `[GC-in-tick]` warnings.

Three fixes:

1. **Codegen** (`PropertyCodec.cs::EmitListWrite` / `EmitDictWrite`):
   change `foreach (var __v in {prop}.Items)` to
   `foreach (var __v in {prop})`.  `ObservableList<T>` and
   `ObservableDict<K,V>` both expose a public instance
   `GetEnumerator()` returning the underlying `List<T>.Enumerator`
   struct, so duck-typed `foreach` resolves to the struct overload —
   no IEnumerable interface dispatch, no enumerator boxing.

2. **`Ops` / `OpKeys` / `OpValues`** on both observable containers:
   wrap the underlying `List<T>` in a new `ReadOnlyListView<T>` struct.
   The wrapper preserves the read-only contract (no Add / Clear surface,
   `internal` constructor) and a `List<T>.Enumerator GetEnumerator()`
   so codegen's `foreach` stays allocation-free.  It also implements
   `IReadOnlyList<T>` so cold paths that need an interface (LINQ helpers,
   xUnit's `Assert.All`) keep working — those box the wrapper, but the
   codegen hot path doesn't take that branch.

3. **GC-in-tick observability** (`Lifecycle.cs`): the existing
   `[GC-in-tick]` warning correlates wall-clock spikes inside
   `PublishReplicationAll` with `GC.CollectionCount` deltas.  This now
   reads as the steady-state regression alarm.

### Verified results

200-client × 120 s aggressive baseline (5–30 s churn, 5 m walk, 5%
teleport):

| | Before | After |
|---|---|---|
| `IEnumerable<T>` boxing time (60 s `dotnet-trace gc-verbose`) | 8485 ms | **0 ms** |
| `[GC-in-tick]` warnings (120 s baseline) | 3 (clustered at 5.7 s / 10.4 s / 65–68 s) | 0–1 (noise) |
| `Script.PublishReplicationAll` mean | 664 μs | **627 μs** |
| `Script.PublishReplicationAll` max | 6,590 μs (recurring) | 7,712 μs (single, ramp-up only) |
| Recurring mid-run spikes | yes | **none** |

The single remaining 7.7 ms spike at t≈6.6 s is a one-time cold-start
event — Tier-0 → Tier-1 JIT promotion on first full property serialization,
combined with first-touch type-metadata loading.  The next tick (t=6.694 s)
runs in 49 μs.  No subsequent tick exceeds 1.6 ms.  Acceptable for
production servers that run for hours / days.

154/154 `Atlas.Runtime.Tests` pass (incl. ListSync / DictSync /
NestedContainer suites that read `.Ops` via `Assert.All`).

## Remaining work (deferred)

These were on the original solution list but **not** shipped yet.
They were deliberately deferred because the boxing fix alone got the
recurring spikes to zero — no business-critical signal pushes for more
optimisation right now.

### Batch P/Invoke

100 individual `NativeApi.PublishReplicationFrame` calls per tick at
~50–200 ns each plus GC-transition costs.  Replace with a single
batched call:

```csharp
// Current: 100 P/Invokes per tick
NativeApi.PublishReplicationFrame(entityId, ...);  // × 100

// Proposed: 1 P/Invoke per tick
NativeApi.PublishReplicationFrameBatch(ReadOnlySpan<ReplicationFrameEntry> frames);
```

Estimated saving: ~50 % of mean `PublishReplicationAll` time at 100+
entities.  Cost: stable memory layout across the managed/native
boundary, batched reception path on the C++ side.

### Skip unchanged entities

`PublishReplicationAll` already short-circuits at the publish step
when `BuildAndConsumeReplicationFrame` returns "no change", but the
serialization itself still runs.  Move a dirty-flag check ahead of
`BuildAndConsumeReplicationFrame` so a static entity (no property
changes, no events this tick) is skipped entirely.

Estimated saving: proportional to the fraction of static entities
in the scene.  In a steady-state social hub or PvE town this can be
80 %+ of the entity count.

### One-time ramp-up spike

The 7.7 ms cold-start spike is a JIT / type-metadata first-touch
event.  Mitigations from cheapest to most invasive:

| Approach | Win | Cost |
|---|---|---|
| Accept (current) | — | 0 |
| Cellapp init runs a synthetic warm tick that exercises every serializer | spike moves out of the operator-visible window | ~50 LOC of warmup harness |
| `DOTNET_TieredCompilation=0` (Tier-1 only) | spike disappears entirely | start-up time +1–2 s |
| ReadyToRun AOT pre-compile of `Atlas.Runtime` + game assemblies | startup serves native code, zero JIT | build complexity, dll size ~2× |

At MMO uptimes (hours / days) a 7.7 ms tick at boot is invisible; not
worth fixing today.

## Tuning cookbook

| Symptom | Likely cause | Knob |
|---|---|---|
| `[GC-in-tick]` recurring (≥ 1/30 s sustained) | Allocations creeping back into the codegen hot path (a new collection accessor returning an interface, a property setter that boxes) | `dotnet-trace --providers "Microsoft-DotNETCore-SampleProfiler,Microsoft-Windows-DotNETRuntime:0x40200001:5" --duration 00:01:00`; convert to speedscope; grep frames for `IEnumerable / IEnumerator` |
| `Script.PublishReplicationAll` mean drifts up | New properties on hot entity types (more bytes/tick) | Profile the type with a `[MemoryDiagnoser]` BenchmarkDotNet harness against a representative call shape |
| `Script.PublishReplicationAll` p99 / max climbs | Per-tick allocation crept in; gen0 → gen1 promotion | `dotnet-counters monitor --counters gen-0-gc-count,gen-1-gc-count,gc-pause-time-ratio,alloc-rate` while running the baseline; cross-reference timestamps with Tracy spike events |
| Single max-spike at startup | Cold JIT / ReadyToRun gap — see deferred work above | none required for production |

## Key files

- `src/csharp/Atlas.Runtime/Entity/EntityManager.cs` — `PublishReplicationAll`
- `src/csharp/Atlas.Runtime/Entity/ServerEntity.cs` — `BuildAndConsumeReplicationFrame`
- `src/csharp/Atlas.Runtime/Core/Lifecycle.cs` — `PublishReplicationAllWithGCProbe` and the `[GC-in-tick]` warning
- `src/csharp/Atlas.Generators.Def/Emitters/PropertyCodec.cs` — `EmitListWrite`, `EmitDictWrite`, `EmitListOpsWrite`, `EmitDictOpsWrite`
- `src/csharp/Atlas.Shared/Observable/ObservableList.cs` — `Items`, `Ops`, `OpValues` accessors
- `src/csharp/Atlas.Shared/Observable/ObservableDict.cs` — `Items`, `Ops`, `OpKeys`, `OpValues` accessors
- `src/csharp/Atlas.Shared/Observable/ReadOnlyListView.cs` — read-only zero-alloc wrapper

## Risks (for the deferred batch P/Invoke)

- Batch P/Invoke requires the C++ side to accept a variable-length
  frame array; memory layout must be stable across the managed / native
  boundary.
- Codegen changes affect every generated entity type; the existing
  `Atlas.Runtime.Tests` ListSync / DictSync / NestedContainerSync
  suites must be regenerated and re-run before shipping.
