# Property Dirty-Flag Tracking

**Priority:** P0
**Subsystem:** `src/server/cellapp/cell_entity.h`, C# script layer
**Impact:** Serialization cost reduction of 30-50% for mostly-static entities

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
