using System;

namespace Atlas.Observable;

// Rebinding contract for Observable containers nested inside another
// container. Parents call __Rebind on adoption so the child's dirty path
// reaches the parent's MarkChildDirty(slot/key); detach rebinds to a
// no-op so orphans stop firing. The `__` prefix flags this as
// generator-adjacent plumbing, not a script-facing API.
public interface IObservableChild
{
    void __Rebind(Action markDirty);

    // Drains the child's pending ops because the parent's outer op already
    // ships the integral state on the wire. Without this, next tick would
    // re-emit folded-in mutations as new deltas.
    void __ForceClear();
}

// Symmetric hook for generated structs that carry Observable container
// fields — when the struct lands in a parent slot, the parent calls
// __RebindObservableFields so the inner container's dirty path bubbles
// up to that slot.
public interface IRebindableFields
{
    void __RebindObservableFields(Action markDirty);
}
