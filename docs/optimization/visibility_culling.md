# Visibility Culling

**Status:** ⚪ Open — game-design dependent. Defer until combat /
team / fog-of-war specs land.
**Subsystem:** `src/server/cellapp/witness.cc`, AoI trigger.

## What this would be

AoI today is a pure distance check: every entity within radius is
visible at full fidelity. Many designs need cheaper variants:

- **Fog of war** — enemies invisible until scouted by allies.
- **Stealth / invisibility** — selective per-observer hiding.
- **View cone** — only entities in front of the camera updated at
  full rate.
- **Team filtering** — friendlies sync fewer property fields when
  the UI already covers them via local state.

The hook is a pluggable `IVisibilityFilter` checked inside
`Witness::Update()` before per-peer work, plus an optional
`PropertyMask` for partially-visible peers. Filter implementations
live on the C# script side, registered per-Space or per-entity-type.

## Caveats (when implementing)

- Filter runs per-peer per-observer per-tick. Must be O(1) — table
  lookup or bitmask, not raycasting or geometry queries.
- Hiding an entity emits an AoI leave event; the client must handle
  the apparent disappearance gracefully without breaking prediction.
- Filters must be deterministic. Server hides + client expects
  visible = desync.
