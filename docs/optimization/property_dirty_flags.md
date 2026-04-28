# Property Dirty-Flag Tracking

**Status:** ✅ Shipped (different shape than originally proposed —
dirty tracking lives entirely on the C# side; no native bitset, no
`MarkPropertyDirty` P/Invoke).
**Priority:** ~~P0~~ done
**Subsystem:** `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs`,
`src/csharp/Atlas.Generators.Def/Emitters/PropertiesEmitter.cs`,
generated entity partials
**Impact:** Only changed properties are serialized per tick; per-audience
masks split scalar / container sections so default-zero containers cost
1 byte (sectionMask) instead of a per-property tag.

> **What we shipped:** the source generator emits a per-entity
> `_dirtyFlags` field (sized to 8/16/32/64 bits depending on property
> count) plus four compile-time audience masks
> (`OwnerVisibleScalarMask`, `OwnerVisibleContainerMask`,
> `OtherVisibleScalarMask`, `OtherVisibleContainerMask`). Each
> property setter ANDs in its `ReplicatedDirtyFlags` bit; container
> properties bubble up via the observable container's mutation hooks.
> `BuildAndConsumeReplicationFrame` short-circuits with `return false`
> when both `_dirtyFlags` and `_volatileDirty` are zero — the C++ side
> never sees a frame for unchanged entities.
>
> **Wire format** (`SerializeOwnerDelta` / `SerializeOtherDelta`): one
> sectionMask byte (bit0 = scalars, bit1 = containers, bit2 =
> components), then a flag word + only the changed fields per section.
> Effectively the "dirty mask + changed-only" layout the proposal
> asked for, but generated per audience instead of using a single
> 64-bit cross-audience bitmap.
>
> **Why no native bitset:** the C++ side doesn't read individual
> property values, only the opaque delta byte stream produced by C#.
> Tracking dirty state on both sides would have required a synchronised
> mirror with no benefit. Keeping it C#-only also lets the codegen
> tailor the flag word width per entity type (saves bytes for
> small entities) and keeps `[EntityProperty]` setters from crossing
> the managed/native boundary on every assignment.

## Current Behavior

The C# script layer (`Atlas.Runtime`) calls `PublishReplicationFrame` every
tick to emit a delta for each entity. The C++ side blindly ships whatever the
script provides — it has no visibility into which properties actually changed.

In a 100v100 battle, the majority of entities only change position each tick.
Combat properties (hp, buffs, status) change infrequently. Yet every entity
generates a full delta frame regardless.

## Problem

- Serialization cost is paid for unchanged properties.
- Network bandwidth carries zero-diff bytes.
- The all-zero delta skip in `Witness::Update` catches the case where
  *nothing* changed, but not the common case where *only position* changed.

## Proposed Solution

### C++ Side

Add a per-property dirty bitset to `CellEntity`:

```cpp
class CellEntity {
  // ...
  std::bitset<64> dirty_props_;  // up to 64 properties per entity type

  void MarkDirty(uint16_t prop_index) { dirty_props_.set(prop_index); }
  void ClearDirty() { dirty_props_.reset(); }
  bool AnyDirty() const { return dirty_props_.any(); }
  auto DirtyMask() const -> uint64_t { return dirty_props_.to_ullong(); }
};
```

### C# Side

`[EntityProperty]` setter calls a native API to mark the property dirty:

```csharp
[EntityProperty(index = 3)]
public int Hp {
    get => _hp;
    set {
        if (_hp == value) return;
        _hp = value;
        NativeApi.MarkPropertyDirty(EntityId, 3);
    }
}
```

### Replication Path

`PublishReplicationFrame` reads the dirty mask and only serializes changed
properties. The delta envelope carries the mask as a header so the receiver
knows which fields follow.

## Wire Format Change

```
Before:  [event_seq] [all_property_bytes...]
After:   [event_seq] [dirty_mask:u64] [changed_property_bytes...]
```

This is a breaking wire format change. Bump the ABI version
(`kAtlasAbiVersion` in `clr_export.h`).

## Key Files

- `src/server/cellapp/cell_entity.h` — Add dirty bitset
- `src/lib/clrscript/native_api_provider.h` — Add `MarkPropertyDirty` API
- `src/lib/clrscript/clr_native_api.cc` — Export to C#
- `src/csharp/Atlas.Runtime/` — Property setter codegen
- `src/server/cellapp/real_entity_data.cc` — Delta serialization

## Risks

- Requires all property mutations to go through setters. Direct field writes
  bypass dirty tracking.
- 64-property limit per entity type. Exceeding this needs a larger bitset or
  multi-word mask.
