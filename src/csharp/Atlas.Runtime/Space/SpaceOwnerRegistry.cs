using System;
using System.Collections.Generic;

namespace Atlas.Space;

// Per-cellapp lookup space_id → owner CellSpaceEntity. Single-threaded
// (cellapp script thread); a duplicate Register with a new entity throws.
public static class SpaceOwnerRegistry
{
    private static readonly Dictionary<uint, CellSpaceEntity> s_owners = new();

    public static void Register(uint spaceId, CellSpaceEntity entity)
    {
        if (s_owners.TryGetValue(spaceId, out var existing) && existing != entity)
        {
            throw new InvalidOperationException(
                $"Space {spaceId} already owned by entity {existing.EntityId}; "
                + $"cannot register {entity.EntityId}");
        }
        s_owners[spaceId] = entity;
    }

    public static void Unregister(uint spaceId, CellSpaceEntity entity)
    {
        if (s_owners.TryGetValue(spaceId, out var existing) && existing == entity)
            s_owners.Remove(spaceId);
    }

    public static CellSpaceEntity? Find(uint spaceId) =>
        s_owners.TryGetValue(spaceId, out var e) ? e : null;

    // Test hook only; production reads go through Find/Register.
    public static void ClearForTest() => s_owners.Clear();
}
