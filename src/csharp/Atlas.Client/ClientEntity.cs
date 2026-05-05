using System;
using System.Collections.Generic;
using Atlas.Components;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Serialization;

namespace Atlas.Client;

public abstract class ClientEntity
{
    public uint EntityId { get; internal set; }
    public bool IsDestroyed { get; internal set; }

    // Sticky flag set when an Apply* threw mid-delivery; cleared by a fresh AoI enter or baseline.
    public bool IsCorrupted { get; internal set; }

    public abstract string TypeName { get; }

    // Generator-assigned type index, mirrors server TypeId; used by component RPC stubs.
    public virtual ushort TypeId => 0;

    public ulong LastEventSeq { get; private set; }
    public ulong EventSeqGapsTotal { get; private set; }

    public Vector3 Position { get; private set; }
    public Vector3 Direction { get; private set; }
    public bool OnGround { get; private set; }

    public virtual void Deserialize(ref SpanReader reader) { }

    // Snapshot applies write directly to backing fields; OnXxxChanged is suppressed by design.
    public virtual void ApplyOwnerSnapshot(ref SpanReader reader) { }
    public virtual void ApplyOtherSnapshot(ref SpanReader reader) { }
    public virtual void ApplyReplicatedDelta(ref SpanReader reader) { }

    public virtual void ApplyPositionUpdate(Vector3 pos, Vector3 dir, bool onGround)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientApplyPositionUpdate);
        Position = pos;
        Direction = dir;
        OnGround = onGround;
        OnPositionUpdated(pos);
    }

    // Distinct from OnXxxChanged: high-frequency channel, kept off the per-field callback path.
    protected internal virtual void OnPositionUpdated(Vector3 newPos) { }

    internal void NoteIncomingEventSeq(ulong seq)
    {
        // Reliable delta is ordered; drop out-of-order / duplicates to avoid underflow.
        if (seq <= LastEventSeq) return;

        if (LastEventSeq > 0 && seq > LastEventSeq + 1)
        {
            ulong missed = seq - LastEventSeq - 1;
            EventSeqGapsTotal += missed;
            ClientLog.Warn(
                $"[{TypeName}:{EntityId}] event_seq gap: last={LastEventSeq} got={seq} missed={missed}");
            // Clamp to u32 — a single jump >4G is paging-worthy, not silent-dropping.
            uint reportDelta = missed > uint.MaxValue ? uint.MaxValue : (uint)missed;
            ClientHost.ReportEventSeqGap(EntityId, reportDelta);
        }
        LastEventSeq = seq;
    }

    protected internal virtual void OnInit() { }
    // Fires once after factory + initial transform + other-scope snapshot are all in place.
    protected internal virtual void OnEnterWorld() { }
    protected internal virtual void OnDestroy() { }

    protected internal void SendCellRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        ClientHost.SendCellRpc(EntityId, (uint)rpcId, payload,
                               (ulong)Atlas.Diagnostics.TraceContext.Current);
    }

    protected internal void SendBaseRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        ClientHost.SendBaseRpc(EntityId, (uint)rpcId, payload,
                               (ulong)Atlas.Diagnostics.TraceContext.Current);
    }

    // _replicated[0] is reserved for the entity body so wire componentIdx=0 keeps its meaning.
    // public mirrors ServerEntity._replicated for cross-assembly dispatcher reads — scripts use the typed accessors.
    public ClientReplicatedComponent?[]? _replicated;
    private Dictionary<Type, ClientLocalComponent>? _clientLocal;

    protected virtual int SyncedSlotCount => 0;
    protected virtual int ResolveSyncedSlot(Type componentType) => -1;

    public T AddComponent<T>() where T : ClientReplicatedComponent, new()
    {
        var slot = ResolveSyncedSlot(typeof(T));
        if (slot <= 0)
            throw new InvalidOperationException(
                $"{typeof(T).Name} is not declared as a Synced component on {GetType().Name}");
        _replicated ??= new ClientReplicatedComponent?[SyncedSlotCount + 1];
        if (slot >= _replicated.Length)
            throw new InvalidOperationException(
                $"Slot {slot} for {typeof(T).Name} exceeds declared SyncedSlotCount={SyncedSlotCount}");
        if (_replicated[slot] is T existing) return existing;

        var c = new T();
        c.__Bind(this, slot);
        _replicated[slot] = c;
        c.OnAttached();
        return c;
    }

    public T? GetSyncedComponent<T>() where T : ClientReplicatedComponent
    {
        var slot = ResolveSyncedSlot(typeof(T));
        if (slot <= 0 || _replicated == null || slot >= _replicated.Length) return null;
        return _replicated[slot] as T;
    }

    public bool RemoveComponent<T>() where T : ClientReplicatedComponent
    {
        var slot = ResolveSyncedSlot(typeof(T));
        if (slot <= 0 || _replicated == null || slot >= _replicated.Length) return false;
        if (_replicated[slot] is not T c) return false;
        c.OnDetached();
        _replicated[slot] = null;
        return true;
    }

    public T AddLocalComponent<T>() where T : ClientLocalComponent, new()
    {
        _clientLocal ??= new();
        if (_clientLocal.TryGetValue(typeof(T), out var existing)) return (T)existing;
        var c = new T();
        c._entity = this;
        _clientLocal[typeof(T)] = c;
        c.OnAttached();
        return c;
    }

    public T? GetLocalComponent<T>() where T : ClientLocalComponent
    {
        if (_clientLocal == null) return null;
        return _clientLocal.TryGetValue(typeof(T), out var c) ? (T)c : null;
    }

    public bool RemoveLocalComponent<T>() where T : ClientLocalComponent
    {
        if (_clientLocal == null) return false;
        if (!_clientLocal.TryGetValue(typeof(T), out var c)) return false;
        c.OnDetached();
        _clientLocal.Remove(typeof(T));
        return true;
    }

    // Synced components first (slot order, deterministic) then ClientLocal (insertion order).
    protected internal void TickAllComponents(float deltaTime)
    {
        if (_replicated != null)
        {
            for (int i = 1; i < _replicated.Length; ++i)
                _replicated[i]?.OnTick(deltaTime);
        }
        if (_clientLocal != null)
        {
            foreach (var c in _clientLocal.Values) c.OnTick(deltaTime);
        }
    }
}
