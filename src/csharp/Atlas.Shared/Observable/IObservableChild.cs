using System;

namespace Atlas.Observable;

// Rebinding contract for observable containers that can be nested inside
// another container. When a parent container adopts a child (via Add /
// Insert / indexer-set / AddWithoutDirty / SetWithoutDirty), it calls
// `__Rebind` so the child's dirty notification reaches the parent's
// MarkChildDirty(slot/key), which propagates up to the entity. Detach
// on eviction rebinds to a no-op so orphaned children stop firing.
//
// The `__` prefix flags this as generator-adjacent plumbing, not a
// script-facing API — scripts should never call __Rebind directly.
public interface IObservableChild
{
    void __Rebind(Action markDirty);

    // Called by a parent when its outer op (Set / Splice / Clear) already
    // carries this child's current state on the wire. The child drains
    // its own op log and recurses — any still-pending ops are now moot
    // because the outer payload ships the integral state. Without this
    // drain, next tick would re-emit stale inner ops that duplicate
    // already-delivered changes.
    void __ForceClear();
}

// Symmetric hook for generated structs that carry Observable container
// fields (e.g., a struct with a `list[int]` field). When such a struct
// lands in an outer ObservableList slot, the outer calls
// `__RebindObservableFields` so the inner containers' dirty path bubbles
// up to the outer slot. Struct is a value type but its container fields
// are references, so copies share the same ObservableList — re-binding
// on the containing field reflects through every copy.
public interface IRebindableFields
{
    void __RebindObservableFields(Action markDirty);
}
