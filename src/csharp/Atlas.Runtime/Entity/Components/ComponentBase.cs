namespace Atlas.Entity.Components;

// Root of the Component class hierarchy. Holds the back-reference to the
// owning entity and the lifecycle hooks every Component shares; the
// three concrete bases (ReplicatedComponent, ServerLocalComponent,
// ClientLocalComponent) layer on synchronization / locality concerns
// without duplicating the lifecycle plumbing.
public abstract class ComponentBase
{
    internal ServerEntity _entity = null!;

    public ServerEntity Entity => _entity;

    public virtual void OnAttached() { }
    public virtual void OnDetached() { }
    public virtual void OnTick(float deltaTime) { }
}
