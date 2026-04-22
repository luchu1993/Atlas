using System;
using Atlas.Core;
using Atlas.DataTypes;
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

    /// <summary>
    /// Build and consume this entity's replication frame for the current tick.
    /// The source generator emits an override for entities with any replicable
    /// property; the base implementation returns <see langword="false"/> so
    /// entities without client-visible state pay no cost.
    /// <para/>
    /// Called once per tick by the CellApp replication pump, <b>after</b>
    /// user <see cref="OnTick"/> code has had the chance to mutate
    /// properties and flip volatile dirty bits. Clears both the dirty
    /// flags and the volatile dirty flag as a side effect of consumption
    /// — hence "AndConsume".
    /// <para/>
    /// ZERO-ALLOC CONTRACT: the four <see cref="SpanWriter"/>s are caller-
    /// owned; they MUST be reset before the call (so stale bytes from a
    /// prior entity don't bleed in) and the caller consumes
    /// <c>.WrittenSpan</c> before the next Reset/Dispose. The pump is
    /// expected to pool one writer quartet across all entities per tick,
    /// calling <c>Reset()</c> between each. See
    /// phase10_cellapp.md §3.3 / §3.9 for the data-flow contract.
    /// </summary>
    /// <param name="ownerSnapshot">Full-state snapshot for the owner audience. Written only when event_seq advances.</param>
    /// <param name="otherSnapshot">Full-state snapshot for observer audience.</param>
    /// <param name="ownerDelta">Audience-filtered delta for owner — <see cref="PropertyScope.OwnClient"/>, <see cref="PropertyScope.AllClients"/>, <see cref="PropertyScope.CellPublicAndOwn"/>, <see cref="PropertyScope.BaseAndClient"/>.</param>
    /// <param name="otherDelta">Audience-filtered delta for observers — <see cref="PropertyScope.AllClients"/>, <see cref="PropertyScope.OtherClients"/> only.</param>
    /// <param name="eventSeq">Property-event sequence; 0 if no property changed this tick.</param>
    /// <param name="volatileSeq">Volatile (pos/orient) sequence; 0 if no volatile change.</param>
    /// <returns>
    /// <c>true</c> when at least one stream advanced (event_seq and/or
    /// volatile_seq > 0 in the outputs). <c>false</c> is the fast path — the
    /// pump should NOT hand zero-length spans to NativeApi in that case.
    /// </returns>
    public virtual bool BuildAndConsumeReplicationFrame(
        ref SpanWriter ownerSnapshot, ref SpanWriter otherSnapshot,
        ref SpanWriter ownerDelta, ref SpanWriter otherDelta,
        out ulong eventSeq, out ulong volatileSeq)
    {
        eventSeq = 0;
        volatileSeq = 0;
        return false;
    }

    // Transform state — lives on the volatile channel (kEntityPositionUpdate
    // envelope), not the replicated-property channel. The Position setter
    // both syncs C++ CellEntity::position_ via AtlasSetEntityPosition (so
    // AoI triggers see the move in the same tick) and flips
    // VolatileDirtyCore (so the next BuildAndConsumeReplicationFrame
    // advances VolatileSeq). Direction / OnGround piggyback on the volatile
    // seq — Witness reads them directly when volatile_seq advances.
    //
    // See PROPERTY_SYNC_DESIGN.md §9.3 "Position 走独立通道".
    public Vector3 Position
    {
        get => _position;
        set
        {
            if (_position != value)
            {
                _position = value;
                NativeApi.SetEntityPosition(EntityId, value);
                _volatileDirty = true;
            }
        }
    }

    public Vector3 Direction
    {
        get => _direction;
        set
        {
            if (_direction != value)
            {
                _direction = value;
                _volatileDirty = true;
            }
        }
    }

    public bool OnGround
    {
        get => _onGround;
        set
        {
            if (_onGround != value)
            {
                _onGround = value;
                _volatileDirty = true;
            }
        }
    }

    private Vector3 _position;
    private Vector3 _direction;
    private bool _onGround;

    /// <summary>
    /// Mark this entity's position/orientation as dirty for the current tick.
    /// The next <see cref="BuildAndConsumeReplicationFrame"/> will advance
    /// <see cref="ReplicationFrame.VolatileSeq"/> and clear this flag.
    /// <para/>
    /// Typically called transitively via <see cref="Position"/>/<see cref="Direction"/>/
    /// <see cref="OnGround"/> setters; scripts rarely need to invoke it
    /// directly.
    /// </summary>
    public void MarkVolatileDirty() => _volatileDirty = true;

    /// <summary>Current volatile-dirty state (exposed for generated code).</summary>
    protected bool VolatileDirtyCore
    {
        get => _volatileDirty;
        set => _volatileDirty = value;
    }

    private bool _volatileDirty;

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

    /// <summary>
    /// Transfer this entity's client connection to another base entity.
    /// Typical use: <c>Account.SelectAvatar</c> creates the Avatar then
    /// calls <c>GiveClientTo(avatar.EntityId)</c> so subsequent client
    /// RPCs target the avatar. After the call <c>this</c> no longer owns
    /// the proxy.
    /// </summary>
    protected internal void GiveClientTo(uint destEntityId)
    {
        NativeApi.GiveClientTo(EntityId, destEntityId);
    }

    /// <summary>
    /// Adjust this entity's AoI radius + hysteresis on the cell. Only
    /// valid for <c>has_cell</c> entity types after a witness has been
    /// attached (typically after <see cref="GiveClientTo"/> has landed
    /// and the cell has received the EnableWitness); the call is a
    /// logged no-op otherwise. <para/>
    /// Mirrors BigWorld's <c>entity.setAoIRadius(radius, hyst)</c>
    /// (witness.cpp:2109). The cell side clamps radius to
    /// <c>[0.1, cellApp/max_aoi_radius]</c> and uses hysteresis as the
    /// leave-band width (enters fire at <paramref name="radius"/>;
    /// leaves fire at <c>radius + hysteresis</c>). In Atlas — unlike
    /// BigWorld — hysteresis is actually applied to the trigger,
    /// suppressing boundary thrash.
    /// <para/>
    /// Split note: <c>ServerEntity</c> will be split into base/cell
    /// mixins in a later pass — at that point this method moves to the
    /// base-side surface where it naturally lives.
    /// </summary>
    public void SetAoIRadius(float radius, float hysteresis = 5f)
    {
        NativeApi.SetAoIRadius(EntityId, radius, hysteresis);
    }
}
