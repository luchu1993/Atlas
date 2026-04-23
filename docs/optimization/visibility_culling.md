# Visibility Culling

**Priority:** P2
**Subsystem:** `src/server/cellapp/witness.cc`, AoI trigger
**Impact:** Reduces AoI population by 30-70% depending on game design

## Current Behavior

AoI is a pure distance check — every entity within the radius is visible to
every observer. There is no concept of line-of-sight, team affiliation, fog
of war, or view cone.

## Problem

In a 100v100 battle, both teams see all 200 entities at full fidelity. In
many game designs, significant culling is possible:

- **Fog of war:** Enemies not scouted by allies are invisible.
- **Stealth/invisibility:** Specific entities are hidden from specific observers.
- **View cone:** Only entities in front of the camera need full-rate updates.
- **Team filtering:** Friendly entities may need fewer property fields synced
  (no need to send hp to allies who already see the health bar via UI).

## Proposed Solution

Add a pluggable visibility filter to the Witness:

```cpp
class IVisibilityFilter {
 public:
  virtual ~IVisibilityFilter() = default;

  // Return false to skip replicating `target` to `observer` this tick.
  virtual bool IsVisible(const CellEntity& observer,
                         const CellEntity& target) const = 0;

  // Optional: return a reduced property mask for partially-visible entities.
  virtual auto PropertyMask(const CellEntity& observer,
                            const CellEntity& target) const -> uint64_t {
    return ~uint64_t{0};  // all properties by default
  }
};
```

### Integration Point

In `Witness::Update()`, before processing each peer:

```cpp
if (visibility_filter_ && !visibility_filter_->IsVisible(owner_, peer)) {
  continue;  // skip this peer entirely
}
```

The filter is set per-Space or per-entity-type via script configuration.

### Game-Side Implementation (C#)

```csharp
[VisibilityFilter]
public class BattleVisibility : IVisibilityFilter {
    public bool IsVisible(Entity observer, Entity target) {
        // Same team: always visible
        if (observer.TeamId == target.TeamId) return true;
        // Enemy: only if scouted
        return target.IsRevealed;
    }
}
```

## Key Files

- `src/server/cellapp/witness.h` — Add `IVisibilityFilter*` member
- `src/server/cellapp/witness.cc` — Check filter in update loop
- `src/server/cellapp/space.h` — Space-level filter registration
- C# script layer — Filter implementation

## Risks

- Filter runs per-peer per-observer per-tick. Must be O(1) — table lookup
  or bitmask, not raycasting.
- State consistency: if filter hides an entity, the client must handle the
  entity "disappearing" gracefully (AoI leave event).
- Desync risk: filter must be deterministic. If server hides entity but
  client expects it, prediction breaks.
