# Lazy / Compact Baseline Snapshot

**Priority:** P1
**Subsystem:** `src/server/cellapp/witness.cc`, `src/csharp/Atlas.Runtime/`,
`Atlas.Generators.Def` (codegen)
**Impact:** Cut per-entity baseline payload by 30-80%; reduce per-spawn
serialization work proportionally.

## Current Behavior

When an observer enters a peer's AoI, CellApp ships a `kEntityEnter`
envelope followed by `kReplicatedBaselineFromCell` containing the full
state snapshot. The codegen for `BuildAndConsumeReplicationFrame` writes
*every* property and *every* component slot regardless of whether they
hold non-default values.

Concrete impact measured at P3.7 Config-S25:

| Metric | P0 baseline | P3 after | Change |
|---|---|---|---|
| `msg=0xF002` avg per envelope | 6 B | 188 B | **×30** |
| Total baseline bytes/run | 150 B | 4 723 B | ×31 |

The 188 B includes:

- **8 entity-level properties** (hp + mainWeapon struct + scores list +
  resists dict + combos nested list + loadouts dict-of-list + buffs
  list-of-struct + spellSlots dict-of-struct), most empty containers
  serialised as `[u16 count=0]` × 6 = ~12 B of pure framing.
- **1 Synced component** (`StressLoadComponent`) with another 6 properties
  in the same shape mix → another ~50 B even when "empty".
- **Per-component framing**: slot index, section mask, dirty-flag width
  bytes — repeats per slot.

The growth is dominated by **frame overhead on default-valued container
properties**, not by user data.

## Problem

Every entity spawn replays this 188 B baseline to every peer in AoI. With
N entities × M observers each, total baseline traffic is O(N·M·payload).
At 25 clients, peak observed: 4.7 KB just from baselines vs 150 B at P0.

More important than bytes: **serialisation CPU**. Each baseline calls the
property-by-property writer chain, which for empty containers still runs
the auto-bubble check + ObservableList.Items enumeration. P3.7 observed
this contributes to the 200-700 ms dispatcher stalls on entity spawn
storms (see [async_entity_lifecycle.md](async_entity_lifecycle.md) for
the storm context).

## Proposed Solution

Three layered tactics; each can land independently.

### Tactic 1: omit empty containers from the baseline

Wire format gets a per-property "present" bit. Empty containers + default
scalars are skipped on the writer side; the receiver knows from the
property's static schema what the default looks like and doesn't need
the data.

```
Before (per property):
  scalar:    [value bytes]
  container: [u16 count=0]              ← 2 B even when empty

After:
  bitmap header: [u64 present]          ← 1 bit per property
  for each present prop:
    scalar:    [value bytes]
    container: [u16 count][values]
```

Default-valued scalars (0 for numerics, false for bool, "" for strings,
empty struct for kStruct) drop too. Receiver sees a 0-bit and writes the
default. Wire format change → bump baseline ABI.

### Tactic 2: defer component baseline until first delta

Components flagged `lazy="true"` are NOT serialised in the baseline. The
client allocates the component instance only when the first delta for
that slot arrives. Pairs with Phase 3 of
[async_entity_lifecycle.md](async_entity_lifecycle.md) — server-side
allocation also defers.

For `StressLoadComponent` (which mutates over multiple seconds of OnTick),
this means new observers don't pay the snapshot cost on join — they
naturally accumulate the state via subsequent deltas. Trade-off: a brief
window where the observer sees the entity without component fields. For
StressAvatar this is fine; for entities where a component contains
load-bearing state (e.g., player inventory) the flag stays off.

### Tactic 3: shared baseline cache

When the same baseline payload is shipped to multiple observers within a
short window (the typical case during a peer-batch enter), build it once
and reference-count the buffer instead of regenerating per observer.
Witness already has the per-observer subscribe path; the change is
caching the serialised bytes on `CellEntity::cached_baseline_` with a
generation counter that invalidates on any property mutation.

```cpp
class CellEntity {
  std::vector<std::byte> cached_baseline_;
  uint64_t cached_baseline_gen_{0};
  uint64_t props_gen_{0};  // bump on every mutation

  std::span<const std::byte> GetCachedBaseline() {
    if (cached_baseline_gen_ != props_gen_) {
      cached_baseline_.clear();
      BuildBaseline(cached_baseline_);
      cached_baseline_gen_ = props_gen_;
    }
    return cached_baseline_;
  }
};
```

For a 25-observer space, this turns 25 serialise calls into 1 + 24
memcpys.

## Wire Format Change

Tactic 1 is a **breaking change** to the baseline payload — bump the
baseline-section version byte (currently implicit) so old clients refuse
to decode. Tactic 2 reuses the existing component-section bit-2 flag
plus an "absent" bit (currently unused in the baseline-section layout
described in `CONTAINER_PROPERTY_SYNC_DESIGN.md` §7.2). Tactic 3 is
internal; no wire change.

## Key Files

- `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs` —
  `SerializeForOwnerClient` / `SerializeForOtherClients` writers
- `src/csharp/Atlas.Generators.Def/Emitters/ComponentEmitter.cs` —
  per-component baseline (currently writes via `WriteOwnerDelta` semantics)
- `src/server/cellapp/witness.cc` — `SendEntityEnter` / baseline shipping
- `src/server/cellapp/cell_entity.{h,cc}` — `cached_baseline_` storage
  (Tactic 3)
- `entity_defs/*.def` — `lazy="true"` attribute on Synced components
  (Tactic 2)

## Risks

- **Default-value mismatch**: Tactic 1 trusts the receiver to know the
  default for each prop. If the server's default and the client's static
  schema diverge (e.g., codegen mismatch), state desync results without
  any deserialisation error. Add a startup digest-equality check before
  enabling.
- **Lazy-attach race**: Tactic 2 has the same client-side risk as the
  P3.5 ApplyComponentSection auto-attach: a slot id never seen on this
  client is treated as fatal (entity marked corrupted). Need stable
  per-entity-type slot tables on both sides.
- **Cache invalidation cost**: Tactic 3 adds a generation counter bump
  on every property write. For an entity that mutates every tick, the
  cache never hits — same cost as today, plus the bump. For mostly-static
  entities (the common case at scale), it's pure win. The 100v100 design
  target leans toward many static entities once combat settles, so the
  expected hit rate is high.

## Validation

Re-run Config-S25 with comprehensive coverage StressAvatar (P3.5).
Success criteria:

- `msg=0xF002` avg ≤ 60 B (33% of current).
- Total baseline bytes/run cut by ≥ 50%.
- Spawn-storm slow-tick contribution decreases (correlates with
  [async_entity_lifecycle.md](async_entity_lifecycle.md) measurements).
