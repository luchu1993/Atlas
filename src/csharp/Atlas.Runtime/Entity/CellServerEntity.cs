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

    // Pull spawn pos/dir from C++ CellEntity into our backing fields without
    // pushing back through the setters (which would round-trip to the same
    // C++ instance we just read from). Called once at RestoreEntity time.
    internal void PullSpawnTransformFromNative()
    {
        _position = NativeApi.GetEntityPosition(EntityId);
        _direction = NativeApi.GetEntityDirection(EntityId);
    }
}
