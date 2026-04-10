namespace Atlas.Entity;

/// <summary>
/// Base class for all server-side entities. Game scripts derive from this
/// and override <see cref="OnInit"/>, <see cref="OnTick"/>, <see cref="OnDestroy"/>.
/// </summary>
public abstract class ServerEntity
{
    public uint EntityId { get; internal set; }
    public bool IsDestroyed { get; internal set; }

    /// <summary>
    /// Entity type name. Overridden in derived classes (by convention or
    /// future Entity Source Generator) to return a compile-time constant.
    /// Used for serialization, factory creation, and hot-reload state migration.
    /// </summary>
    public abstract string TypeName { get; }

    /// <summary>Called when the entity is first created or after hot-reload.</summary>
    protected internal virtual void OnInit(bool isReload) { }

    /// <summary>Called every server tick.</summary>
    protected internal virtual void OnTick(float deltaTime) { }

    /// <summary>Called when the entity is destroyed or during server shutdown.</summary>
    protected internal virtual void OnDestroy() { }
}
