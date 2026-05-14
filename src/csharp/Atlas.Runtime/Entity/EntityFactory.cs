using System;
using System.Collections.Generic;
using Atlas.Core;
using Atlas.DataTypes;

namespace Atlas.Entity;

/// <summary>
/// Registry of entity creators keyed by type name and 1-based type id.
/// Generator-emitted code registers creators via <see cref="Register"/>
/// from a [ModuleInitializer] when the script assembly loads.
/// <para/>
/// <see cref="CreateBase"/> / <see cref="CreateBaseByTypeId"/> go further:
/// they round-trip through NativeApi.CreateBaseEntity so BaseApp can
/// allocate an EntityID, instantiate the C# side via the RestoreEntity
/// callback, and (for has_cell types) kick off cell entity creation on
/// a CellApp. The returned <see cref="ServerEntity"/> is already
/// registered in <see cref="EntityManager"/> and ready to use.
/// </summary>
public static class EntityFactory
{
    private static readonly Dictionary<string, Func<ServerEntity>> _byName =
        new(StringComparer.Ordinal);
    private static readonly Dictionary<ushort, Func<ServerEntity>> _byTypeId = new();
    private static readonly Dictionary<string, ushort> _typeIdByName =
        new(StringComparer.Ordinal);

    public static void Register(string typeName, ushort typeId, Func<ServerEntity> creator)
    {
        _byName[typeName] = creator;
        _byTypeId[typeId] = creator;
        _typeIdByName[typeName] = typeId;
    }

    // Name → typeId mapping without a creator. Used for types this process
    // can spawn (cell-only) but does not instantiate locally (e.g. base
    // calling SpawnCellOnly for a has_base="false" type).
    public static void RegisterTypeId(string typeName, ushort typeId)
    {
        _typeIdByName[typeName] = typeId;
    }

    public static ServerEntity? Create(string typeName) =>
        _byName.TryGetValue(typeName, out var creator) ? creator() : null;

    public static ServerEntity? CreateByTypeId(ushort typeId) =>
        _byTypeId.TryGetValue(typeId, out var creator) ? creator() : null;

    /// <summary>Returns the 1-based type id registered for a name, or 0 if unknown.</summary>
    public static ushort GetTypeId(string typeName) =>
        _typeIdByName.TryGetValue(typeName, out var id) ? id : (ushort)0;

    /// <summary>
    /// Creates a new base entity of the named type on the local BaseApp
    /// and returns it. <paramref name="spaceId"/> is forwarded to the cell
    /// side for has_cell types (ignored for base-only). Witness attachment
    /// happens later via the client-bind path; call
    /// <see cref="ServerEntity.SetAoIRadius"/> after
    /// <see cref="ServerEntity.GiveClientTo"/> to override the default
    /// AoI radius.
    /// </summary>
    public static BaseServerEntity? CreateBase(string typeName, uint spaceId = 1)
    {
        var typeId = GetTypeId(typeName);
        if (typeId == 0) return null;
        return CreateBaseByTypeId(typeId, spaceId);
    }

    /// <summary>
    /// Creates a new base entity of the given type id on the local BaseApp.
    /// The C# instance is materialised synchronously via the RestoreEntity
    /// callback from within the native call, so it is available in
    /// EntityManager before this method returns. <paramref name="spaceId"/>
    /// is forwarded to the cell side for has_cell types.
    /// </summary>
    public static BaseServerEntity? CreateBaseByTypeId(ushort typeId, uint spaceId = 1)
    {
        var entityId = NativeApi.CreateBaseEntity(typeId, spaceId);
        if (entityId == 0) return null;
        return EntityManager.Instance.Get(entityId) as BaseServerEntity;
    }

    // Cell-script entry: instantiates a cell-only entity locally. No Base
    // counterpart, no DB persistence; call CellServerEntity.DestroySelf to
    // tear it down.
    public static CellServerEntity? CreateLocalCell(string typeName, uint spaceId,
                                                    Vector3 position, Vector3 direction,
                                                    bool onGround = false)
    {
        var typeId = GetTypeId(typeName);
        if (typeId == 0) return null;
        var entityId = NativeApi.CreateLocalCellEntity(typeId, spaceId, position, direction,
                                                       onGround);
        if (entityId == 0) return null;
        return EntityManager.Instance.Get(entityId) as CellServerEntity;
    }

    // Base-script entry: asks a CellApp to spawn a cell-only entity. Returns
    // true if the routing message left the BaseApp; the cell allocates the
    // id asynchronously, so this does not return the new ServerEntity.
    public static bool SpawnCellOnly(string typeName, uint spaceId, Vector3 position,
                                     Vector3 direction, bool onGround = false)
    {
        var typeId = GetTypeId(typeName);
        if (typeId == 0) return false;
        return NativeApi.RequestSpawnCellOnly(typeId, spaceId, position, direction, onGround);
    }
}
