# Script PublishReplicationAll — GC Pressure

**Status:** ✅ Shipped (round 1 — codegen boxing eliminated). Further
reductions (batch P/Invoke, dirty-skip) are deferred until business
complexity grows; see [Deferred work](#deferred-work) below.
**Subsystem:** `src/csharp/Atlas.Runtime/Entity/EntityManager.cs`,
`src/csharp/Atlas.Runtime/Core/Lifecycle.cs`,
`src/csharp/Atlas.Generators.Def/Emitters/PropertyCodec.cs`,
`src/csharp/Atlas.Shared/Observable/`.

## Design

`EntityManager.PublishReplicationAll()` runs once per tick across all
entities, reusing four shared `SpanWriter` instances (zero-alloc by
contract). The hot allocation surface that drove the original
recurring `[GC-in-tick]` warnings was inside the codegen-emitted
`BuildAndConsumeReplicationFrame`: `foreach` over a property exposed
as `IReadOnlyList<T>` / `IReadOnlyDictionary<K,V>` boxed the
otherwise-struct enumerator on every call.

### What's in the codegen now

- **`EmitListWrite` / `EmitDictWrite`** iterate the underlying
  `ObservableList<T>` / `ObservableDict<K,V>` directly. Both expose a
  public instance `GetEnumerator()` returning the underlying
  `List<T>.Enumerator` struct, so duck-typed `foreach` resolves to
  the struct overload — no interface dispatch, no boxing.
- **`Ops` / `OpKeys` / `OpValues` accessors** wrap the underlying
  `List<T>` in a `ReadOnlyListView<T>` struct. The wrapper preserves
  the read-only contract and exposes `List<T>.Enumerator
  GetEnumerator()` for codegen, while still implementing
  `IReadOnlyList<T>` for cold callers (LINQ helpers, test asserts).
  Cold callers box the wrapper; the codegen path doesn't.

### `[GC-in-tick]` observability

`Lifecycle.PublishReplicationAllWithGCProbe` correlates wall-clock
spikes with `GC.CollectionCount` deltas. A re-emerging warning is
the steady-state regression alarm — see the cookbook below.

## Tuning cookbook

| Symptom | Likely cause | Knob |
|---|---|---|
| `[GC-in-tick]` recurring (≥ 1 / 30 s sustained) | Allocation crept back into the codegen hot path | `dotnet-trace --providers "Microsoft-DotNETCore-SampleProfiler,Microsoft-Windows-DotNETRuntime:0x40200001:5" --duration 00:01:00`; convert to speedscope; grep frames for `IEnumerable / IEnumerator` |
| `Script.PublishReplicationAll` mean drifts up | New properties on hot entity types (more bytes/tick) | Profile the type with a `[MemoryDiagnoser]` BenchmarkDotNet harness |
| `Script.PublishReplicationAll` p99 / max climbs | Per-tick allocation crept in; gen0 → gen1 promotion | `dotnet-counters monitor --counters gen-0-gc-count,gen-1-gc-count,gc-pause-time-ratio,alloc-rate`; cross-reference with Tracy spike timestamps |
| Single max-spike at startup | Cold JIT / Tier-1 promotion on first full property serialization | Accept (1-shot at boot, invisible at MMO uptimes) or run a synthetic warmup tick |

## Deferred work

These were on the original solution list but not shipped because the
boxing fix alone got recurring spikes to zero.

### Batch P/Invoke

Today: 100+ individual `NativeApi.PublishReplicationFrame` calls per
tick, each ~50–200 ns plus GC-transition cost. Replace with a single
batched call taking `ReadOnlySpan<ReplicationFrameEntry>`. Estimated
saving: ~50 % of mean `PublishReplicationAll` time at 100+ entities.
Cost: stable managed/native memory layout for the entry struct, plus
a batched-reception path on the C++ side.

### Skip unchanged entities

`PublishReplicationAll` short-circuits at the publish step when the
delta is empty, but serialization still runs. Move the dirty check
ahead of `BuildAndConsumeReplicationFrame` so static entities skip
entirely. Saving is proportional to the static-entity fraction —
80 %+ in social hubs / PvE towns, much less in dense PvP.

## Caveats

- Round 1 closed the recurring spike, not the cold-start spike. A
  one-time JIT promotion at boot is acceptable for MMO uptimes; if a
  future deployment requires zero-jank startup, mitigations are
  warmup tick → `DOTNET_TieredCompilation=0` → ReadyToRun AOT, in
  ascending cost order.
- Codegen changes affect every generated entity type. Re-run the
  `Atlas.Runtime.Tests` ListSync / DictSync / NestedContainerSync
  suites before shipping any new accessor / wrapper changes.
