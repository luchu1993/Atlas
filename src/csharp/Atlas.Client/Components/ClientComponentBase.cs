namespace Atlas.Components;

// Lifecycle base shared by every client-side component flavour. Mirrors
// the server's `Atlas.Entity.Components.ComponentBase` so script code
// reads symmetrically across processes — the difference is the binding
// target (ClientEntity here vs. ServerEntity on the server).
public abstract class ClientComponentBase
{
    internal Atlas.Client.ClientEntity _entity = null!;
    public Atlas.Client.ClientEntity Entity => _entity;

    /// <summary>Called immediately after the component is attached to the entity.</summary>
    public virtual void OnAttached() { }

    /// <summary>Called when the component is removed or the entity is destroyed.</summary>
    public virtual void OnDetached() { }

    /// <summary>
    /// Per-tick hook invoked by ClientEntity.TickAllComponents. Optional —
    /// most client-local components (visual effects, prediction state) do
    /// the work in property-change callbacks, not on a fixed tick.
    /// </summary>
    public virtual void OnTick(float deltaTime) { }
}
