using System;
using System.Collections.Generic;
using Atlas.Core;
using Atlas.DataTypes;
using Atlas.Entity.Components;
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
    /// Tick update interval. OnTick is called every <c>TickInterval</c> engine
    /// ticks instead of every tick. Default 1 = every tick.
    /// <para/>
    /// <b>MUST be set in the entity constructor.</b> EntityManager reads
    /// <c>TickInterval</c> in <c>Create&lt;T&gt;()</c> right after
    /// <c>new T()</c> to assign a staggered <see cref="TickPhase"/>; setting
    /// it later (e.g. in <c>OnInit</c>) leaves all instances of that type
    /// with phase 0, which clusters their ticks on the same frame and
    /// defeats the spreading.
    /// <para/>
    /// Values &lt;= 1 disable spreading and behave as "every tick".
    /// </summary>
    public int TickInterval
    {
        get => _tickInterval;
        protected set => _tickInterval = value < 1 ? 1 : value;
    }
    private int _tickInterval = 1;

    /// <summary>Phase within the tick interval, assigned by EntityManager.</summary>
    internal int TickPhase { get; set; }

    /// <summary>
    /// Entity type name. Overridden by Entity Source Generator to return
    /// the compile-time constant from [Entity("TypeName")].
    /// </summary>
    public abstract string TypeName { get; }

    /// <summary>
    /// Numeric type index assigned by the C# generator (typeIndexMap).
    /// Mirrors the client side; used by component RPC stubs to compose
    /// rpc_id at send time. Generated entity classes override. Public
    /// to dodge CS0507 when derived classes live in a different assembly
    /// (cross-assembly override of `protected internal` collapses to
    /// `protected` and the codegen would have to emit two variants).
    /// </summary>
    public virtual ushort TypeId => 0;

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
    /// Called once per tick by the CellApp replication pump after user
    /// <see cref="OnTick"/> has run. Clears the property-dirty and volatile-
    /// dirty flags as a side effect of consumption — hence "AndConsume".
    /// <para/>
    /// ZERO-ALLOC: the four <see cref="SpanWriter"/>s are caller-owned and
    /// MUST be Reset before the call; the caller consumes
    /// <c>.WrittenSpan</c> before the next Reset/Dispose.
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

    /// <summary>
    /// Slot-bitmap reserved for the future Component pump. Bit N is set
    /// when the Component at slot N has staged a frame's worth of changes.
    /// Stays zero until a Component is attached, so emitter-generated
    /// BuildAndConsumeReplicationFrame overrides can test <c>!= 0</c> and
    /// skip the per-slot iteration at zero runtime cost.
    ///
    /// Kept here rather than in a partial so that any ServerEntity subclass
    /// — regardless of whether the Component pump is wired up — has a
    /// stable type-level hook.
    /// </summary>
    protected internal ulong _dirtyComponents;

    // =========================================================================
    // Component container — the entity owns a static slot table for Synced
    // components plus a Type-keyed dict for ServerLocal components.
    // _replicated[0] is permanently reserved for the entity body so user
    // slot indices start at 1 and componentIdx=0 keeps its meaning on the
    // wire.
    // =========================================================================

    // public so codegen-emitted entity partials AND the cross-assembly
    // RPC dispatcher (DefRpcDispatcher.Dispatch*Rpc) can read it. The
    // field is conceptually internal — scripts shouldn't poke it — but
    // C#'s access rules can't both restrict scripts and let a static
    // dispatcher in another assembly through. Documented as a "do not
    // touch from script code" affordance.
    public ReplicatedComponent?[]? _replicated;
    private Dictionary<Type, ServerLocalComponent>? _serverLocal;

    // Number of Synced slots this entity declares. Subclasses (codegen-
    // emitted partials) override; tests hand-roll the count.
    protected virtual int SyncedSlotCount => 0;

    // Resolves a Synced component Type to its slot index. Returns -1 if
    // the type isn't a declared Synced component on this entity. Codegen
    // overrides for real entities; tests provide their own mapping.
    protected virtual int ResolveSyncedSlot(Type componentType) => -1;

    // Adds a declared Synced component. The slot is determined by the
    // codegen-emitted ResolveSyncedSlot; if the slot is already active,
    // the existing instance is returned (idempotent).
    public T AddComponent<T>() where T : ReplicatedComponent, new()
    {
        var slot = ResolveSyncedSlot(typeof(T));
        if (slot <= 0)
            throw new InvalidOperationException(
                $"{typeof(T).Name} is not declared as a Synced component on {GetType().Name}");
        _replicated ??= new ReplicatedComponent?[SyncedSlotCount + 1];
        if (slot >= _replicated.Length)
            throw new InvalidOperationException(
                $"Slot {slot} for {typeof(T).Name} exceeds declared SyncedSlotCount={SyncedSlotCount}");
        if (_replicated[slot] is T existing) return existing;

        var c = new T();
        c.__Bind(this, slot);
        _replicated[slot] = c;
        _dirtyComponents |= 1UL << slot;
        c.OnAttached();
        return c;
    }

    public T? GetSyncedComponent<T>() where T : ReplicatedComponent
    {
        var slot = ResolveSyncedSlot(typeof(T));
        if (slot <= 0 || _replicated == null || slot >= _replicated.Length) return null;
        return _replicated[slot] as T;
    }

    public bool RemoveComponent<T>() where T : ReplicatedComponent
    {
        var slot = ResolveSyncedSlot(typeof(T));
        if (slot <= 0 || _replicated == null || slot >= _replicated.Length) return false;
        if (_replicated[slot] is not T c) return false;
        c.OnDetached();
        _replicated[slot] = null;
        // Mark dirty so the pump emits the future kRemoveComponent op.
        _dirtyComponents |= 1UL << slot;
        return true;
    }

    public T AddLocalComponent<T>() where T : ServerLocalComponent, new()
    {
        _serverLocal ??= new();
        if (_serverLocal.TryGetValue(typeof(T), out var existing)) return (T)existing;
        var c = new T();
        c._entity = this;
        _serverLocal[typeof(T)] = c;
        c.OnAttached();
        return c;
    }

    public T? GetLocalComponent<T>() where T : ServerLocalComponent
    {
        if (_serverLocal == null) return null;
        return _serverLocal.TryGetValue(typeof(T), out var c) ? (T)c : null;
    }

    public bool RemoveLocalComponent<T>() where T : ServerLocalComponent
    {
        if (_serverLocal == null) return false;
        if (!_serverLocal.TryGetValue(typeof(T), out var c)) return false;
        c.OnDetached();
        _serverLocal.Remove(typeof(T));
        return true;
    }

    // Per-tick component dispatch — Synced first (slot order, deterministic
    // across runs) then ServerLocal (insertion order via Dictionary).
    // Called by the cellapp tick loop AFTER the entity's own OnTick.
    protected internal void TickAllComponents(float deltaTime)
    {
        if (_replicated != null)
        {
            for (int i = 1; i < _replicated.Length; ++i)
                _replicated[i]?.OnTick(deltaTime);
        }
        if (_serverLocal != null)
        {
            foreach (var c in _serverLocal.Values) c.OnTick(deltaTime);
        }
    }

    // Called by ReplicatedComponent.MarkDirty so the entity's
    // _dirtyComponents bitmap reflects the slot's pending state — the
    // replication pump only iterates components when this is non-zero.
    internal void __MarkComponentDirty(int slotIdx)
    {
        _dirtyComponents |= 1UL << slotIdx;
    }

    // Used by tests + future codegen to walk active Synced slots.
    internal ReadOnlySpan<ReplicatedComponent?> ReplicatedSlotsForTest =>
        _replicated == null ? default : _replicated.AsSpan();

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
    /// logged no-op otherwise. The cell side clamps radius to
    /// <c>[0.1, cellApp/max_aoi_radius]</c> and uses hysteresis as the
    /// leave-band width (enters fire at <paramref name="radius"/>;
    /// leaves fire at <c>radius + hysteresis</c>), suppressing boundary
    /// thrash.
    /// </summary>
    public void SetAoIRadius(float radius, float hysteresis = 5f)
    {
        NativeApi.SetAoIRadius(EntityId, radius, hysteresis);
    }
}
