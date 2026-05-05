namespace Atlas.Observable;

// Container-delta op kinds. Capped at 16 to fit a 4-bit wire field —
// today encoded as a full byte, but new kinds still need to fit.
public enum OpKind : byte
{
    // Scalar / struct-whole property update at a specific leaf index.
    Set = 0,

    // list[T]: remove [start, end) then insert N values at start.
    ListSplice = 1,

    // dict[K,V]: insert-or-overwrite key → value.
    DictSet = 2,

    // dict[K,V]: remove key.
    DictErase = 3,

    // Container-wide reset. Absorbs prior ops on the sender so the wire
    // never carries more than one Clear per tick per container.
    Clear = 4,

    // Field-level mutation on a list-of-struct slot.
    StructFieldSet = 5,
}
