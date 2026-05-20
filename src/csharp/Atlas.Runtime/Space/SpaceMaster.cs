using Atlas.Core;

namespace Atlas.Space;

// Public façade for registering the per-cluster space-owner entity type.
// CellAppMgr stamps it on every CreateSpaceRequest so the primary host's
// CellApp auto-spawns the entity at AddCellToSpace time — no script-level
// lazy "first Avatar logs in" hack.
public static class SpaceMaster
{
    public static void Register(uint spaceId, string typeName) =>
        NativeApi.SetSpaceMasterType(spaceId, typeName);
}
