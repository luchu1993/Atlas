# Lazy / Compact Baseline Snapshot

**Status:** 🔵 Deferred — `SendEntityEnter` is 0.34 % of cellapp CPU
at 200 cli; the `cached_*_envelope` mechanism in
[group_broadcast.md](group_broadcast.md) already covers the delta
path. Revisit only if a denser AoI scenario surfaces an enter-storm.
**Subsystem:** `src/server/cellapp/witness.cc`,
`src/csharp/Atlas.Generators.Def/Emitters/{DeltaSyncEmitter,ComponentEmitter}.cs`,
`src/server/cellapp/cell_entity.{h,cc}`.

## What this would be

When a new observer enters AoI, cellapp ships `kEntityEnter` plus
`kReplicatedBaselineFromCell` carrying a full state snapshot. The
codegen writer emits *every* property and component slot regardless
of value — empty containers serialise as a 2-byte count, default
scalars as their full width. With multiple Synced components per
entity, baseline payload is dominated by frame overhead on
default-valued fields, not user data.

Three layered tactics, each landable independently:

1. **Per-property "present" bitmap** — a header bit per property,
   skip default-valued scalars and empty containers entirely. Wire
   format change → bump baseline ABI.
2. **Lazy components** — components flagged `lazy="true"` in the
   `.def` skip the baseline; client allocates the component on the
   first delta. Pairs with the deferred async-spawn work, where
   server-side allocation also defers.
3. **Cached baseline buffer per entity** — first observer in a
   batch builds, subsequent observers memcpy. A generation counter
   on the entity bumps on any mutation; cache invalidates on
   mismatch. Internal-only; no wire change.

## Trigger to revisit

- Sustained `SendEntityEnter` ≥ 5 % of cellapp CPU on a fresh
  capture, or
- An enter-storm scenario (mass spawn, large-scale instance entry,
  raid-mode boss spawn) producing tail-latency spikes traceable to
  baseline cost, or
- A new entity type with > 4 Synced components and dense default
  containers — Tactic 1 alone is likely worth the wire bump.

## Caveats (when implementing)

- Tactic 1 trusts the receiver to know per-property defaults.
  Server / client schema drift causes silent state desync; gate on
  a startup digest-equality check.
- Tactic 2 has the same client-side risk as auto-attach: an unknown
  slot id on the receiver currently flags the entity corrupted.
  Stable per-entity-type slot tables are a prerequisite.
- Tactic 3 adds a generation bump on every property write. For
  entities mutating every tick the cache never hits — same cost
  plus the bump. For mostly-static entities it's pure win, which
  is the common case at scale.
