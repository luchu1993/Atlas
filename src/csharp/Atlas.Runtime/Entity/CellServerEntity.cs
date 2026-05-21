using Atlas.Core;
using Atlas.DataTypes;

namespace Atlas.Entity;

// Cell-resident entity base — owns spatial state and cell-side lifecycle.
public abstract class CellServerEntity : ServerEntity
{
    // Real: setter mirrors to C++ for the same-tick RangeList shuffle and
    // Witness volatile pump; getter returns the cache to avoid per-read
    // P/Invoke. Ghost: C++ side is the source of truth (GhostUpdatePosition
    // wire writes it), so the getter pulls live each call; setter rejects
    // because C++ would reject too.
    public Vector3 Position
    {
        get => IsGhost ? NativeApi.GetEntityPosition(EntityId) : _position;
        set
        {
            if (IsGhost) return;
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
        get => IsGhost ? NativeApi.GetEntityDirection(EntityId) : _direction;
        set
        {
            if (IsGhost) return;
            if (_direction != value)
            {
                _direction = value;
                NativeApi.SetEntityDirection(EntityId, value);
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

    // Cross-cell helpers route here when this entity is a Ghost — the C++
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
        _spaceId = NativeApi.GetEntitySpaceId(EntityId);
    }
}
