using Atlas.Core;
using Atlas.DataTypes;

namespace Atlas.Entity;

// Cell-resident entity base — owns spatial state and cell-side lifecycle.
public abstract class CellServerEntity : ServerEntity
{
    // Setters mirror to C++ CellEntity so the RangeList shuffle + Witness
    // volatile pump see the change in the same tick.
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
