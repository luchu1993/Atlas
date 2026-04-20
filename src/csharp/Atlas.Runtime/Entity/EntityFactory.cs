using System;
using System.Collections.Generic;

namespace Atlas.Entity;

/// <summary>
/// Registry of entity creators keyed by both type name and 1-based type id.
/// Generator-emitted code registers creators via <see cref="Register"/> from
/// a [ModuleInitializer] when the script assembly loads.
/// </summary>
/// <remarks>
/// This replaces the earlier "partial-class shadow" design: prior versions
/// declared <c>partial class EntityFactory</c> here and expected the
/// generator to extend it in the user assembly. That only works within a
/// single assembly; across assemblies the compiler emits two distinct
/// types, and NativeCallbacks.RestoreEntity (in Atlas.Runtime) would bind
/// to this stub's copy — which had no creators and always returned null.
/// Mirrors <see cref="Atlas.Client.ClientEntityFactory"/> on the client side.
/// </remarks>
public static class EntityFactory
{
    private static readonly Dictionary<string, Func<ServerEntity>> _byName =
        new(StringComparer.Ordinal);
    private static readonly Dictionary<ushort, Func<ServerEntity>> _byTypeId = new();

    public static void Register(string typeName, ushort typeId, Func<ServerEntity> creator)
    {
        _byName[typeName] = creator;
        _byTypeId[typeId] = creator;
    }

    public static ServerEntity? Create(string typeName) =>
        _byName.TryGetValue(typeName, out var creator) ? creator() : null;

    public static ServerEntity? CreateByTypeId(ushort typeId) =>
        _byTypeId.TryGetValue(typeId, out var creator) ? creator() : null;
}
