using System;
using System.Collections.Generic;

namespace Atlas.Entity;

/// <summary>
/// Default (empty) entity factory. DefGenerator generates a replacement in
/// consuming projects that registers all entity types from .def files.
/// The consuming .csproj should suppress CS0436 to allow the generated version
/// to shadow this default.
/// </summary>
public static class EntityFactory
{
    public static ServerEntity? Create(string typeName) => null;

    public static ServerEntity? CreateByTypeId(ushort typeId) => null;
}
