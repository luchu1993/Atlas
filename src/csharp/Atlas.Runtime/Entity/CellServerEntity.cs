using Atlas.Core;
using Atlas.DataTypes;

namespace Atlas.Entity;

// Why a dispatched cross-space teleport failed. Values mirror the C++
// atlas::TeleportFailReason enum.
public enum TeleportFailReason : byte
{
    TargetUnhosted = 0,
    SameCellApp = 1,
    NoPeer = 2,
    ResolveTimeout = 3,
    Rejected = 4,
    OffloadTimeout = 5,
}

// Cell-resident entity base; owns spatial state and cell-side lifecycle.
public abstract class CellServerEntity : ServerEntity
{
    // C++ movement/offload can update native pose without touching this cache.
    // Read through native so gameplay queries see the authoritative transform.
    public Vector3 Position
    {
        get
        {
            _position = NativeApi.GetEntityPosition(EntityId);
            return _position;
        }
        set
        {
            if (IsGhost) return;
            Vector3 current = NativeApi.GetEntityPosition(EntityId);
            if (current != value)
            {
                _position = value;
                NativeApi.SetEntityPosition(EntityId, value);
                _volatileDirty = true;
            }
            else
            {
                _position = current;
            }
        }
    }

    public Vector3 Direction
    {
        get
        {
            _direction = NativeApi.GetEntityDirection(EntityId);
            return _direction;
        }
        set
        {
            if (IsGhost) return;
            Vector3 current = NativeApi.GetEntityDirection(EntityId);
            if (current != value)
            {
                _direction = value;
                NativeApi.SetEntityDirection(EntityId, value);
                _volatileDirty = true;
            }
            else
            {
                _direction = current;
            }
        }
    }

    public bool OnGround
    {
        get
        {
            _onGround = NativeApi.GetEntityOnGround(EntityId);
            return _onGround;
        }
        set
        {
            if (IsGhost) return;
            bool current = NativeApi.GetEntityOnGround(EntityId);
            if (current != value)
            {
                _onGround = value;
                NativeApi.SetEntityOnGround(EntityId, value);
                _volatileDirty = true;
            }
            else
            {
                _onGround = current;
            }
        }
    }

    public void SetMovementIntent(Vector3 direction, float speedMps, ushort buttons = 0)
    {
        if (IsGhost || IsDestroyed) return;
        NativeApi.SetMovementIntent(EntityId, direction.X, direction.Z, speedMps, buttons);
    }

    public bool SetMovementCommand(MovementCommand command)
    {
        if (IsGhost || IsDestroyed) return false;
        return NativeApi.SetMovementCommand(EntityId, command);
    }

    public bool ClearMovementCommand(uint commandId = 0)
    {
        if (IsGhost || IsDestroyed) return false;
        return NativeApi.ClearMovementCommand(EntityId, commandId);
    }

    // Cross-space teleport to a hosted space; the move completes async once
    // CellAppMgr names the host. False if Ghost/destroyed or dispatch failed.
    public bool Teleport(uint targetSpaceId, Vector3 position, Vector3 direction)
    {
        if (IsGhost || IsDestroyed) return false;
        return NativeApi.TeleportEntity(EntityId, targetSpaceId, position, direction);
    }

    // Override to react to an async teleport failure (target unhosted, resolve
    // timeout, destination reject, etc.); the entity stays in its current space.
    protected virtual void OnTeleportFailed(TeleportFailReason reason) { }

    internal void DispatchTeleportFailed(TeleportFailReason reason)
    {
        if (!IsDestroyed) OnTeleportFailed(reason);
    }

    public static bool RegisterMovementCurve(ushort curveId, System.ReadOnlySpan<float> samples)
    {
        return NativeApi.SetMovementCurve(curveId, samples);
    }

    public static bool LoadCollisionAsset(uint spaceId, string path)
    {
        return NativeApi.LoadCollisionAsset(spaceId, path);
    }

    public static bool LoadNavMesh(uint spaceId, string collisionPath, string paramsPath)
    {
        return NativeApi.LoadNavMesh(spaceId, collisionPath, paramsPath);
    }

    public bool TryGetMovementHistorySample(uint serverTick, out MovementHistorySample sample)
    {
        if (NativeApi.TryGetMovementHistorySample(
                EntityId, serverTick, out uint sampleTick, out Vector3 position,
                out Vector3 velocity, out Vector3 direction, out uint flags,
                out uint lastSeq))
        {
            sample = new MovementHistorySample(
                sampleTick, position, velocity, direction, flags, lastSeq);
            return true;
        }

        sample = default;
        return false;
    }

    private Vector3 _position;
    private Vector3 _direction;
    private bool _onGround;

    // Typically called transitively via the setters; scripts rarely invoke
    // it directly.
    public void MarkVolatileDirty() => _volatileDirty = true;

    protected bool VolatileDirtyCore
    {
        get => _volatileDirty;
        set => _volatileDirty = value;
    }

    private bool _volatileDirty;

    // Native side fires AoI exit + invokes OnDestroy; refuses base-owned entities.
    public void DestroySelf() => NativeApi.DestroyCellEntity(EntityId);

    // Cross-cell helpers route here when this entity is a Ghost; the C++
    // provider's SendCellRpc rejects Real, so callers don't double-check.
    public void InvokeCellMethodFromGhost(int rpcId, System.ReadOnlySpan<byte> payload)
        => SendCellRpc(rpcId, payload);

    // Live native read keeps the value in sync with Offload / cross-space
    // migration; cache backs OnDestroy, which runs after C++ erases the entity.
    public uint SpaceId
    {
        get
        {
            var live = NativeApi.GetEntitySpaceId(EntityId);
            if (live != 0) _spaceId = live;
            return _spaceId;
        }
    }

    private uint _spaceId;

    internal void PullSpawnTransformFromNative()
    {
        _position = NativeApi.GetEntityPosition(EntityId);
        _direction = NativeApi.GetEntityDirection(EntityId);
        _onGround = NativeApi.GetEntityOnGround(EntityId);
        _spaceId = NativeApi.GetEntitySpaceId(EntityId);
    }
}
