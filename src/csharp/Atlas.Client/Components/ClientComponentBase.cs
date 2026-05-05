namespace Atlas.Components;

// Mirrors server-side ComponentBase; binding target is ClientEntity instead of ServerEntity.
public abstract class ClientComponentBase
{
    internal Atlas.Client.ClientEntity _entity = null!;
    public Atlas.Client.ClientEntity Entity => _entity;

    public virtual void OnAttached() { }
    public virtual void OnDetached() { }
    public virtual void OnTick(float deltaTime) { }
}
