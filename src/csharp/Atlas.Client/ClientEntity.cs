using System;
using System.Collections.Generic;
using Atlas.Client.Native;
using Atlas.Components;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Serialization;

namespace Atlas.Client;

public abstract class ClientEntity
{
    private const uint kMovementFlagGrounded = 1u;

    public uint EntityId { get; internal set; }
    public bool IsDestroyed { get; internal set; }

    // Sticky flag set when an Apply* threw mid-delivery; cleared by a fresh AoI enter or baseline.
    public bool IsCorrupted { get; internal set; }

    // Owner = local player on this client. Owner trusts server snapshots directly
    // (auth movement); peers ride the AvatarFilter ring and lerp on the client tick.
    public bool IsOwner { get; internal set; }
    internal ClientSession? Session { get; set; }

    public abstract string TypeName { get; }

    // Generator-assigned type index, mirrors server TypeId; used by component RPC stubs.
    public virtual ushort TypeId => 0;

    public ulong LastEventSeq { get; private set; }
    public ulong EventSeqGapsTotal { get; private set; }

    public Vector3 Position { get; private set; }
    public Vector3 Direction { get; private set; }
    public bool OnGround { get; private set; }
    public double LastPositionServerTime { get; private set; }
    // Wall-clock seconds at last ApplyPositionUpdate (same domain as WallNowSeconds).
    public double LastPositionWallTime { get; private set; }

    private static readonly System.Diagnostics.Stopwatch s_wallClock =
        System.Diagnostics.Stopwatch.StartNew();

    private readonly MovementCommandPlayback _remoteCommand = new();
    private bool _hasOpenRemoteCommand;
    private bool _hasRemoteCommandPosition;
    private bool _remoteCommandHasServerDirection;

    public static double WallNowSeconds() => s_wallClock.Elapsed.TotalSeconds;

    public AvatarFilter? Filter { get; internal set; }
    public bool HasActiveMovementCommand => _hasOpenRemoteCommand;

    public virtual void Deserialize(ref SpanReader reader) { }

    // Snapshot applies write directly to backing fields; OnXxxChanged is suppressed by design.
    public virtual void ApplyOwnerSnapshot(ref SpanReader reader) { }
    public virtual void ApplyOtherSnapshot(ref SpanReader reader) { }
    public virtual void ApplyReplicatedDelta(ref SpanReader reader) { }

    // Owner snaps to authoritative state; peers feed the filter ring (initialised lazily
    // on first sample) and the rendered transform reads back via TryGetInterpolated.
    public virtual void ApplyPositionUpdate(double serverTime, Vector3 pos, Vector3 dir,
                                            bool onGround)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientApplyPositionUpdate);
        Position = pos;
        Direction = dir;
        OnGround = onGround;
        LastPositionServerTime = serverTime;
        LastPositionWallTime = s_wallClock.Elapsed.TotalSeconds;

        if (!IsOwner)
        {
            if (_hasOpenRemoteCommand)
            {
                ResetInterpolation();
                if (_remoteCommand.IsActive)
                {
                    _remoteCommand.AlignToPosition(pos);
                    ApplyRemoteMovementCommandSample(0);
                    if (_remoteCommand.AllowsTurnInput)
                    {
                        Direction = dir;
                        OnGround = onGround;
                        _remoteCommandHasServerDirection = true;
                    }
                    return;
                }
                _hasRemoteCommandPosition = true;
                OnPositionUpdated(pos);
                return;
            }

            _hasRemoteCommandPosition = false;
            _remoteCommandHasServerDirection = false;
            Filter ??= new AvatarFilter();
            Filter.Input(serverTime, pos, dir, onGround);
        }

        OnPositionUpdated(pos);
    }

    public bool TryGetInterpolated(out Vector3 pos, out Vector3 dir, out bool onGround)
    {
        if (_hasOpenRemoteCommand || _hasRemoteCommandPosition)
        {
            pos = Position;
            dir = Direction;
            onGround = OnGround;
            return true;
        }

        if (Filter is { SampleCount: > 0 })
            return Filter.TryEvaluate(out pos, out dir, out onGround);
        pos = Position;
        dir = Direction;
        onGround = OnGround;
        return false;
    }

    // Server-authoritative teleport: drop the filter buffer so the next sample
    // restarts the interpolation window without lerping from a stale position.
    public void ResetInterpolation()
    {
        Filter?.Reset();
    }

    public bool ApplyMovementCommandStart(MovementCommandStart start)
    {
        if (start.EntityId != EntityId || IsOwner) return false;
        if (!_remoteCommand.Start(start.Command)) return false;

        _hasOpenRemoteCommand = true;
        _remoteCommandHasServerDirection = false;
        ResetInterpolation();
        ApplyRemoteMovementCommandSample(0);
        return true;
    }

    public bool ApplyMovementCommandEnd(MovementCommandEnd end)
    {
        if (end.EntityId != EntityId || IsOwner || end.CommandId == 0) return false;
        if (_remoteCommand.CommandId != 0 && _remoteCommand.CommandId != end.CommandId)
            return false;

        _remoteCommand.Clear();
        _hasOpenRemoteCommand = false;
        _remoteCommandHasServerDirection = false;
        Position = end.State.Position;
        Direction = end.State.Direction;
        OnGround = (end.State.Flags & kMovementFlagGrounded) != 0;
        _hasRemoteCommandPosition = true;
        ResetInterpolation();
        OnPositionUpdated(Position);
        return true;
    }

    public void UpdateInterpolation(float dt)
    {
        if (_remoteCommand.IsActive)
        {
            int deltaMs = Math.Clamp((int)MathF.Round(MathF.Max(dt, 0.0f) * 1000.0f),
                                     0, ushort.MaxValue);
            ApplyRemoteMovementCommandSample((uint)deltaMs);
        }
        Filter?.UpdateLatency(dt);
    }

    void ApplyRemoteMovementCommandSample(uint deltaMs)
    {
        _remoteCommand.AdvanceMs(deltaMs);
        Position = _remoteCommand.Position;
        _hasRemoteCommandPosition = true;
        if (_remoteCommand.TryGetDirection(out var direction) &&
            (!_remoteCommand.AllowsTurnInput || !_remoteCommandHasServerDirection))
            Direction = direction;
        OnPositionUpdated(Position);
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
            Log.Warning($"[{TypeName}:{EntityId}] event_seq gap: last={LastEventSeq} "
                        + $"got={seq} missed={missed}");
            // Clamp to u32; a single jump >4G is paging-worthy, not silent-dropping.
            uint reportDelta = missed > uint.MaxValue ? uint.MaxValue : (uint)missed;
            if (Session != null)
                Session.ReportEventSeqGap(EntityId, reportDelta);
            else
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
        ulong traceId = (ulong)Atlas.Diagnostics.TraceContext.Current;
        if (Session != null)
            Session.SendCellRpc(EntityId, (uint)rpcId, payload, traceId);
        else
            ClientHost.SendCellRpc(EntityId, (uint)rpcId, payload, traceId);
    }

    protected internal void SendBaseRpc(int rpcId, ReadOnlySpan<byte> payload)
    {
        ulong traceId = (ulong)Atlas.Diagnostics.TraceContext.Current;
        if (Session != null)
            Session.SendBaseRpc(EntityId, (uint)rpcId, payload, traceId);
        else
            ClientHost.SendBaseRpc(EntityId, (uint)rpcId, payload, traceId);
    }

    protected internal void SendMovementInput(ReadOnlySpan<AtlasMovementInputFrame> frames)
    {
        if (Session != null)
            Session.SendMovementInput(EntityId, frames);
        else
            ClientHost.SendMovementInput(EntityId, frames);
    }

    protected internal void SendMovementCorrectionReport(uint ackedInputSeq, uint serverTick,
                                                         float distanceM,
                                                         ushort correctionFlags)
    {
        if (Session != null)
        {
            Session.SendMovementCorrectionReport(EntityId, ackedInputSeq, serverTick,
                                                 distanceM, correctionFlags);
        }
        else
        {
            ClientHost.SendMovementCorrectionReport(EntityId, ackedInputSeq, serverTick,
                                                    distanceM, correctionFlags);
        }
    }

    // _replicated[0] is reserved for the entity body; wire componentIdx=0 keeps its meaning.
    // Public mirrors ServerEntity._replicated for cross-assembly dispatcher reads.
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
            throw new InvalidOperationException($"Slot {slot} for {typeof(T).Name} "
                                                + "exceeds declared SyncedSlotCount="
                                                + $"{SyncedSlotCount}");
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

    internal void DetachAllComponents()
    {
        if (_replicated != null)
        {
            for (int i = 1; i < _replicated.Length; ++i)
            {
                _replicated[i]?.OnDetached();
                _replicated[i] = null;
            }
        }
        if (_clientLocal == null) return;
        var localComponents = new List<ClientLocalComponent>(_clientLocal.Values);
        foreach (var c in localComponents) c.OnDetached();
        _clientLocal.Clear();
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
