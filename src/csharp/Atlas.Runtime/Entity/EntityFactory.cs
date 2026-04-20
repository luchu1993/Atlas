using System;
using System.Collections.Generic;
using Atlas.Core;

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

    public static ServerEntity? Create(string typeName) =>
        _byName.TryGetValue(typeName, out var creator) ? creator() : null;

    public static ServerEntity? CreateByTypeId(ushort typeId) =>
        _byTypeId.TryGetValue(typeId, out var creator) ? creator() : null;

    /// <summary>Returns the 1-based type id registered for a name, or 0 if unknown.</summary>
    public static ushort GetTypeId(string typeName) =>
        _typeIdByName.TryGetValue(typeName, out var id) ? id : (ushort)0;

    /// <summary>
    /// Creates a new base entity of the named type on the local BaseApp
    /// and returns it. Same effect as <see cref="CreateBaseByTypeId"/>
    /// but resolves the type name via the local registry first.
    /// </summary>
    public static ServerEntity? CreateBase(string typeName)
    {
        var typeId = GetTypeId(typeName);
        if (typeId == 0) return null;
        return CreateBaseByTypeId(typeId);
    }

    /// <summary>
    /// Creates a new base entity of the given type id on the local BaseApp.
    /// The C# instance is materialised synchronously via the RestoreEntity
    /// callback from within the native call, so it is available in
    /// EntityManager before this method returns.
    /// </summary>
    public static ServerEntity? CreateBaseByTypeId(ushort typeId)
    {
        var entityId = NativeApi.CreateBaseEntity(typeId);
        if (entityId == 0) return null;
        return EntityManager.Instance.Get(entityId);
    }
}
