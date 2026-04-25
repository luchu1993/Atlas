namespace Atlas.Observable;

// Op kinds emitted on the container-delta wire. Values are reserved
// for the 4-bit external bit-pack encoding documented in
// docs/property_sync/CONTAINER_PROPERTY_SYNC_DESIGN.md — the cap at 16
// values means new kinds need to fit in 4 bits. Wire width today is a
// full byte; the bit-pack lands together with reliable-channel framing.
public enum OpKind : byte
{
    // Scalar / struct-whole property update at a specific leaf index.
    Set = 0,

    // list<T>: remove [start, end) then insert N values at start. A Set
    // is the more compact encoding for in-place index assignment.
    ListSplice = 1,

    // dict<K,V>: insert-or-overwrite key → value.
    DictSet = 2,

    // dict<K,V>: remove key.
    DictErase = 3,

    // Container-wide reset. Absorbs prior ops on the sender so the wire
    // never carries more than one Clear per tick per container.
    Clear = 4,

    // Reserved for struct field-sync mode (not yet implemented).
    StructFieldSet = 5,

    // Reserved for Component slot mutations (not yet implemented).
    AddComponent = 6,
    RemoveComponent = 7,
}
