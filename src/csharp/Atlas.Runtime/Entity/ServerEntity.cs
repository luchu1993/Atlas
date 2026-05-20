using System;
using System.Collections.Generic;
using Atlas.Core;
using Atlas.Coro;
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

    // BigWorld-style: true on every cellapp that holds a C++ Ghost mirror.
    // Real-only side effects (movement, witness mods, controllers) must gate on
    // IsReal; cross-cell exposed methods route to the Real owner transparently.
    public bool IsGhost { get; internal set; }
    public bool IsReal => !IsGhost;

    // Cancelled on destroy / offload / hot-reload. RPCs that pass this
    // token complete with RpcErrorCodes.Cancelled instead of stranding.
    private readonly AtlasCancellationSource _lifecycle = new();
    public AtlasCancellationToken LifecycleCancellation => _lifecycle.Token;
    internal void TriggerLifecycleCancellation() => _lifecycle.Cancel();

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
    /// hasEvent/hasVolatile drive the C++ runtime's monotonic seq counters
    /// (replication_state_.latest_event_seq / latest_volatile_seq). Keeping
    /// the seqs in the runtime is what makes Offload work without resets.
    public virtual bool BuildAndConsumeReplicationFrame(
        ref SpanWriter ownerSnapshot, ref SpanWriter otherSnapshot,
        ref SpanWriter ownerDelta, ref SpanWriter otherDelta,
        out bool hasEvent, out bool hasVolatile)
    {
        hasEvent = false;
        hasVolatile = false;
        return false;
    }

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

    /// <summary>Called when this cellapp gains a Ghost mirror of the entity.
    /// OnInit does NOT fire on Ghosts — Real-only setup (movement controllers,
    /// witness, projectile-target registration on the Real path) stays out of
    /// Ghost paths. Override for Ghost-side scripting (e.g. registering the
    /// Ghost as a projectile target so a cross-cell hit routes via exposed
    /// cell method).</summary>
    protected internal virtual void OnGhostInit() { }

    // C++ controller state migrates on offload, but Action delegates are
    // managed — scripts must re-StartTimer in OnInit(isReload=true).
    private Dictionary<int, (bool Repeat, Action Callback)>? _timers;
    private int _nextTimerArg;

    /// <summary>Schedules a per-entity timer. Returns a handle for CancelTimer; 0 on failure.</summary>
    public long StartTimer(float intervalSeconds, bool repeat, Action callback)
    {
        if (callback is null) throw new ArgumentNullException(nameof(callback));
        var userArg = ++_nextTimerArg;
        var controllerId = NativeApi.AddTimerController(EntityId, intervalSeconds, repeat, userArg);
        if (controllerId == 0) return 0;
        _timers ??= new();
        _timers[userArg] = (repeat, callback);
        return ((long)controllerId << 32) | (uint)userArg;
    }

    public void CancelTimer(long handle)
    {
        if (handle == 0) return;
        var controllerId = (int)(handle >> 32);
        var userArg = (int)(handle & 0xFFFFFFFF);
        NativeApi.CancelController(EntityId, controllerId);
        _timers?.Remove(userArg);
    }

    internal void DispatchTimerFired(int userArg)
    {
        if (_timers == null) return;
        if (!_timers.TryGetValue(userArg, out var entry)) return;
        if (!entry.Repeat) _timers.Remove(userArg);
        entry.Callback();
    }

    // =========================================================================
    // RPC send infrastructure — called by generated Mailbox proxies and stubs.
    // Each method forwards to the C++ engine via NativeApi.
    // =========================================================================

    protected internal void SendClientRpc(int rpcId, RpcTarget target,
        ReadOnlySpan<byte> payload)
    {
        NativeApi.SendClientRpc(EntityId, (uint)rpcId, target, payload,
                                (ulong)Atlas.Diagnostics.TraceContext.Current);
    }

    protected internal void SendCellRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        NativeApi.SendCellRpc(EntityId, (uint)rpcId, payload,
                              (ulong)Atlas.Diagnostics.TraceContext.Current);
    }

    protected internal void SendBaseRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        NativeApi.SendBaseRpc(EntityId, (uint)rpcId, payload,
                              (ulong)Atlas.Diagnostics.TraceContext.Current);
    }
}
