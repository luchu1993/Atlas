using Atlas.Entity;

namespace Atlas.Space;

// Cell-side base for the per-space owner entity. OnInit / OnDestroy are
// sealed and drive SpaceOwnerRegistry; scripts override OnSpaceInit instead.
public abstract class CellSpaceEntity : CellServerEntity
{
    protected internal override sealed void OnInit(bool isReload)
    {
        SpaceOwnerRegistry.Register(SpaceId, this);
        OnSpaceInit(isReload);
    }

    protected internal override sealed void OnDestroy()
    {
        try { OnSpaceDestroy(); }
        finally { SpaceOwnerRegistry.Unregister(SpaceId, this); }
    }

    protected virtual void OnSpaceInit(bool isReload) { }
    protected virtual void OnSpaceDestroy() { }
}
