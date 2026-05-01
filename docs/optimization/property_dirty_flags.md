# Property Dirty-Flag Tracking

**Status:** ✅ Shipped (codegen-only; no native bitset).
**Subsystem:** `src/csharp/Atlas.Generators.Def/Emitters/DeltaSyncEmitter.cs`,
`src/csharp/Atlas.Generators.Def/Emitters/PropertiesEmitter.cs`,
generated entity partials.

## Design

Dirty tracking lives entirely on the C# side — the source generator
emits per-entity flag plumbing; the native side only sees the opaque
delta byte stream and never reads individual property values, so a
mirrored bitset on the C++ side would be redundant.

### Generated artefacts

- **`_dirtyFlags` field** — sized to 8/16/32/64 bits depending on
  property count.
- **Compile-time audience masks** — `OwnerVisibleScalarMask`,
  `OwnerVisibleContainerMask`, `OtherVisibleScalarMask`,
  `OtherVisibleContainerMask`. Per-property bits are pre-baked into
  these masks so audience splits cost zero per-tick work.
- **Setters** — each `[EntityProperty]` setter ANDs in its
  `ReplicatedDirtyFlags` bit.
- **Container properties** — bubble up via the observable container's
  mutation hooks.
- **`BuildAndConsumeReplicationFrame`** — short-circuits with
  `return false` when both `_dirtyFlags` and `_volatileDirty` are
  zero, so unchanged entities never produce a frame.

### Wire format

Inside `SerializeOwnerDelta` / `SerializeOtherDelta`:

```
[sectionMask: u8]   bit0=scalars, bit1=containers, bit2=components
{ [flagWord] [changedFieldBytes…] }   per present section
```

This is the dirty-mask + changed-only layout we wanted, generated per
audience instead of via a single cross-audience bitmap. Per-entity
flag-word width keeps the framing cheap on small entities.

## Caveats

- Direct field writes bypass the setter and skip dirty marking.
  `[EntityProperty]` codegen is the only blessed mutation path.
- Container properties depend on observable wrappers
  (`ObservableList<T>` / `ObservableDict<K,V>`) for bubble-up; raw
  `List<T>` mutations are invisible.
- Adding a 9th / 17th / 33rd / 65th property to a hot entity type
  promotes the flag word to the next size class — verify the
  generated emit path.
