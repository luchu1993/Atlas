using System;
using Atlas.Core;
using Atlas.Serialization;

namespace Atlas.Entity;

/// <summary>
/// Base class for all server-side entities. Game scripts derive from this
/// and override <see cref="OnInit"/>, <see cref="OnTick"/>, <see cref="OnDestroy"/>.
/// Source Generators extend this via partial classes to add serialization,
/// dirty tracking, RPC stubs, and mailbox proxies.
/// </summary>
public abstract class ServerEntity
{
    public uint EntityId { get; internal set; }
    public bool IsDestroyed { get; internal set; }

    /// <summary>
    /// Entity type name. Overridden by Entity Source Generator to return
    /// the compile-time constant from [Entity("TypeName")].
    /// </summary>
    public abstract string TypeName { get; }

    /// <summary>Full entity state for persistence and hot-reload (implemented by source generator).</summary>
    public abstract void Serialize(ref SpanWriter writer);

    /// <summary>Restores entity state (implemented by source generator).</summary>
    public abstract void Deserialize(ref SpanReader reader);

    /// <summary>
    /// Owner-client-scope snapshot: fields visible to the client that owns this
    /// entity. Used by BaseApp's periodic baseline pump. The source generator
    /// overrides this for entities that expose owner-visible properties;
    /// entities without any owner-visible fields inherit the default no-op.
    /// </summary>
    public virtual void SerializeForOwnerClient(ref SpanWriter writer) { }

    /// <summary>
    /// Other-clients-scope snapshot: fields visible to non-owning observers.
    /// Reserved for future CellApp AOI use; the source generator overrides this
    /// for entities with other-visible properties.
    /// </summary>
    public virtual void SerializeForOtherClients(ref SpanWriter writer) { }

    /// <summary>Called when the entity is first created or after hot-reload.</summary>
    protected internal virtual void OnInit(bool isReload) { }

    /// <summary>Called every server tick.</summary>
    protected internal virtual void OnTick(float deltaTime) { }

    /// <summary>Called when the entity is destroyed or during server shutdown.</summary>
    protected internal virtual void OnDestroy() { }

    // =========================================================================
    // RPC send infrastructure — called by generated Mailbox proxies and stubs.
    // Each method forwards to the C++ engine via NativeApi.
    // =========================================================================

    protected internal void SendClientRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        NativeApi.SendClientRpc(EntityId, (uint)rpcId, payload);
    }

    protected internal void SendCellRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        NativeApi.SendCellRpc(EntityId, (uint)rpcId, payload);
    }

    protected internal void SendBaseRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        NativeApi.SendBaseRpc(EntityId, (uint)rpcId, payload);
    }
}
