# Async Entity Init / Teardown

**Priority:** P1
**Subsystem:** `src/server/cellapp/cellapp.cc`, `src/lib/clrscript/`, BaseApp / CellApp dispatch loop
**Impact:** Eliminate dispatcher stalls during session lifecycle storms (1-6 sec slow ticks → sub-100ms)

## Current Behavior

The cellapp dispatcher event loop processes inbound packets serially. When a
session lifecycle message arrives (CreateCellEntity / DestroyCellEntity /
EntityTransferred), the handler runs to completion before the dispatcher can
service the next packet — including the next tick's `AdvanceTime`. The
heavyweight steps inside that handler are:

1. **Allocate `CellEntity`** + insert into populations / spaces.
2. **Allocate component instances** (`AddComponent<T>` for each Synced /
   ServerLocal slot in the entity's def).
3. **Build and ship the baseline** snapshot to AoI peers (`kEntityEnter` +
   `kReplicatedBaselineFromCell`). With P3.5 the baseline grew from 6 B avg
   to 188 B avg per entity (×30) because every component now contributes a
   slot record.
4. **Subscribe to AoI** triggers, recompute every existing witness's enter
   set so the new entity's baseline is queued for them.
5. **CLR call into C# `OnInit`** — script-side allocations, possible JIT,
   possible GC.

Steps 2-5 are all synchronous and serial. Total per-entity cost rose from
~5 ms (P0) to ~30-80 ms (P3.5) measured indirectly via slow-tick deltas.

## Problem

When a session storm arrives — empirically observed at the
`hold-min-ms` boundary in `world_stress`, but also reproducible during normal
mass login at startup — the dispatcher is stuck for the duration of all
queued lifecycle events. Concrete data from P3.7 profiling on Config-S25:

- Steady-state per-tick work: 2.5 ms (healthy, 2.5% util).
- Lifecycle-storm slow ticks: 200 ms / 1059 ms / 2129 ms (consecutive,
  cumulative 3.4 s of dispatcher starvation).
- Echo RTT p99 regressed 30 ms → 176 ms — Echo packets queued behind the
  blocked handler.

Staggering session disconnect (mitigation A) made the situation **worse**,
because the same total work was distributed across more stall events. The
fix has to be on per-event cost, not burst pattern.

## Proposed Solution

### Phase 1: yield the dispatcher between heavy phases

Split `OnCreateCellEntity` and `OnDestroyCellEntity` so the scheduler can
process one inbound packet per dispatcher iteration, not N per:

```cpp
// Today:
void CellApp::OnCreateCellEntity(const CreateCellEntity& msg) {
  // 1. Allocate CellEntity
  // 2. AddComponent<T>() for each slot via CLR
  // 3. Build baseline + ship to peers
  // 4. AoI subscribe / recompute witnesses
  // 5. Call OnInit on C# entity
}  // dispatcher blocked the entire time

// Proposed:
void CellApp::OnCreateCellEntity(const CreateCellEntity& msg) {
  auto* e = AllocateAndRegister(msg);
  // Defer the rest to the post-tick "lifecycle pump" — each tick processes
  // at most N pending entities, so a 25-entity storm spreads across 25/N
  // ticks rather than blocking one.
  pending_lifecycle_.push_back({e, LifecyclePhase::kInitComponents});
}
```

The pump runs at the head of every `OnTickComplete`, processing up to
`config_.lifecycle_per_tick_budget` entries. Default = 4-8.

### Phase 2: parallelise non-replicating phases

Component allocation + `OnInit` script call don't touch shared state until
the entity is added to AoI subscriptions. They can run on a worker thread
pool:

```cpp
// Worker: alloc components, call OnInit, build baseline payload bytes.
std::async([e]() {
  e->AttachDeclaredComponents();
  e->RunScriptOnInit();
  e->BuildBaselinePayloadCached();
});

// Main thread later: subscribe to AoI, ship the cached baseline.
```

Wire this in only after Phase 1 lands and proves stable — the threading
contract for `OnInit` (which scripts may use to access `EngineContext`)
needs review first.

### Phase 3: lazy component attach

For components flagged `lazy="true"` in the .def, skip allocation in OnInit
and let the first `AddComponent<T>()` call (or the first delta arrival on
the client) instantiate. Pairs with the baseline-shrink strategy in
[lazy_baseline.md](lazy_baseline.md) — empty slots emit `[u8 slot_idx][u8
sectionMask=0]` instead of a full snapshot.

## Wire Format Change

None for Phase 1-2. Phase 3 adds an "absent" sectionMask bit (already
reserved as bit 7 in the baseline section; see CONTAINER_PROPERTY_SYNC_DESIGN
§7.2) so receivers don't speculatively allocate the component instance until
the first real delta arrives.

## Key Files

- `src/server/cellapp/cellapp.cc` — `OnCreateCellEntity`, new pump
- `src/server/cellapp/cellapp.h` — pending-lifecycle queue
- `src/server/cellapp/cellapp_native_provider.cc` — CLR `OnInit` invocation
  separation from baseline build
- `src/lib/clrscript/clr_script_engine.cc` — thread-affinity contract for
  scripts (Phase 2 only)
- `entity_defs/*.def` — `lazy="true"` attribute on selected components
  (Phase 3 only)

## Risks

- **Visibility ordering**: scripts that observe a peer entity's existence
  via AoI before its `OnInit` has run will see uninitialised state. Phase 1
  must keep the "fully constructed before first witness sees it" invariant
  by holding the AoI publish until all phases on the deferred path complete.
- **Phase 2 thread safety**: `EngineContext.SyncContext` is currently
  single-threaded. Async `OnInit` would need a marshaling boundary back to
  the main thread for any property mutation that happens inside `OnInit`.
- **Phase 3 dirty-bit ambiguity**: a never-attached lazy component vs an
  attached but never-modified component look identical on the wire if
  bit-7-absent isn't honored on both ends.

## Validation

Re-run Config-S25 with both burst (`hold-min/max=15000/15000`) and staggered
(`8000/22000`) session patterns. Success criteria:

- Slow-tick count ≤ 1 across 30 s.
- Echo RTT p99 ≤ 50 ms (within 2× of P0 baseline 30 ms).
- `bytes_rx_per_cli_s` regression bounded (the comprehensive coverage in
  P3.5 will keep this above the P0 887 B/s baseline; aim for ≤ 2× of P0).
