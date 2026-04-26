# Script PublishReplicationAll — GC Pressure & Allocation Audit

**Priority:** P1
**Subsystem:** `src/csharp/Atlas.Runtime/Entity/EntityManager.cs`,
`src/csharp/Atlas.Runtime/Core/Lifecycle.cs`
**Impact:** Eliminate GC-induced spike in `Script.PublishReplicationAll`;
reduce managed allocation rate on the tick hot path

## Profiling Evidence

Baseline run (100 clients, 120 s, `b70b0ad`):

| Metric | Value |
|---|---|
| `Script.PublishReplicationAll` mean | 307 μs |
| `Script.PublishReplicationAll` p99 | 896 μs |
| `Script.PublishReplicationAll` max | **6,590 μs** |
| Ticks > 1 ms | 7 / 723 (0.97%) |
| Ticks > 2 ms | 3 / 723 (0.41%) |
| Max spike tick offset | ~5.7 s from capture start |

The max=6.59 ms spike is 7× the p99 value, indicating a **discrete pause
event** rather than algorithmic slowness. Spike timestamps cluster at ramp-up
(5.7 s, 10.4 s) and again at 65–68 s into the run — patterns consistent with
.NET gen0/gen1 GC pauses triggered by allocation pressure.

The `Script.OnTick` max (7.77 ms) is almost entirely explained by this spike
(`PublishReplicationAll` max = 6.59 ms = 84.8% of `Script.OnTick` max).

## Current Behavior

`EntityManager.PublishReplicationAll()` runs once per tick across all 100
entities:

```csharp
// EntityManager.PublishReplicationAll — called every tick
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

The `SpanWriter` quartet is already shared across entities (zero-alloc
contract). However, generated `BuildAndConsumeReplicationFrame` overrides
may allocate for:

- `List<T>` / `Dictionary<K,V>` property serialization (boxing, enumerator
  structs promoted to heap, LINQ)
- Any `string` formatting in property getters
- `event_seq` / `volatile_seq` delta computation if it allocates intermediate
  state

Additionally, `_entities.Values` iteration on `Dictionary<uint, ServerEntity>`
allocates a `ValueCollection.Enumerator` on the heap in frameworks < .NET 9
(in .NET 9 this is a struct enumerator — verify).

## Problem

1. **GC pressure from generated code**: the codegen for
   `BuildAndConsumeReplicationFrame` is not audited for zero-alloc compliance.
   Any collection property that boxes or creates enumerator objects on the
   heap accumulates across 100 entities × 10 Hz = 1000 calls/s.

2. **P/Invoke overhead**: 100 `NativeApi.PublishReplicationFrame` calls per
   tick. Each P/Invoke has a fixed overhead (~50–200 ns) plus GC transition
   cost if the call pins managed memory.

3. **Ramp-up spike**: at 5.7 s (ramp-up phase), all 100 entities are created
   within a few ticks. Object graph allocation during `OnInit` may trigger a
   gen1 or gen2 collection that lands mid-tick.

## Proposed Solution

### 1. GC Monitoring Pass (prerequisite)

Run `dotnet-counters` in parallel with the next baseline to correlate GC
pause timestamps with `Script.PublishReplicationAll` spike timestamps:

```bash
dotnet-counters monitor --process-id <cellapp-PID> \
    System.Runtime --counters \
    gen-0-gc-count,gen-1-gc-count,gen-2-gc-count,gc-pause-time-ratio,\
    alloc-rate,loh-size
```

Map the output timestamps against Tracy's `Script.PublishReplicationAll`
spike at t=5.7 s / t=10.4 s. If GC pause timestamps align, confirm root cause.

### 2. Allocation Audit of Generated Code

Add `[MemoryDiagnoser]` BenchmarkDotNet benchmark for a representative entity
type (`StressAvatar`) calling `BuildAndConsumeReplicationFrame` in a loop.
Identify and fix any per-call allocation:

- Replace `IEnumerable<T>` returns with `ReadOnlySpan<T>` or
  pre-allocated arrays where possible.
- Ensure all collection properties use value-type enumerators.
- Verify no boxing in property serialization paths.

### 3. Batch P/Invoke

Replace 100 individual `NativeApi.PublishReplicationFrame` P/Invoke calls
with a single batched call that passes all dirty-entity frames in one crossing:

```csharp
// Current: 100 P/Invokes per tick
NativeApi.PublishReplicationFrame(entityId, ...);  // × 100

// Proposed: 1 P/Invoke per tick
NativeApi.PublishReplicationFrameBatch(ReadOnlySpan<ReplicationFrameEntry> frames);
```

This also removes the per-call GC transition cost and improves cache locality
on the C++ side (one contiguous buffer vs. 100 scattered calls).

### 4. Skip Unchanged Entities

Combined with `property_dirty_flags.md`: if an entity has no dirty properties
and no events this tick, skip `BuildAndConsumeReplicationFrame` entirely.
`PublishReplicationAll` already has a fast path for entities where the frame
returns `false` (no change) — but the serialization still runs. Move the
dirty check before serialization.

## Key Files

- `src/csharp/Atlas.Runtime/Entity/EntityManager.cs` — `PublishReplicationAll`
- `src/csharp/Atlas.Runtime/Entity/ServerEntity.cs` — `BuildAndConsumeReplicationFrame`
- `src/csharp/Atlas.Generators.Def/` — codegen templates for replication frame
- `src/lib/clrscript/clr_script_engine.cc` — `NativeApi.PublishReplicationFrame` P/Invoke binding

## Expected Impact

| Change | Expected reduction |
|---|---|
| GC audit + zero-alloc fix | Eliminate spike > 2 ms (0.41% of ticks) |
| Batch P/Invoke | ~50% reduction in mean PublishReplicationAll time |
| Skip unchanged entities | Proportional to fraction of static entities |

At 100 clients the mean (307 μs) is acceptable; the goal is eliminating the
tail spikes (max 6.59 ms) that contribute to `Script.OnTick` jitter.

## Risks

- Batch P/Invoke requires C++ side to accept variable-length frame array;
  memory layout must be stable across the managed/native boundary.
- Codegen changes affect all generated entity types; regression test coverage
  must be verified before shipping.
