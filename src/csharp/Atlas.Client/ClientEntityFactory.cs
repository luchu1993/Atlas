using System;
using System.Collections.Generic;

namespace Atlas.Client;

public static class ClientEntityFactory
{
    private static readonly Dictionary<ushort, Func<ClientEntity>> s_creators = new();

    public static void Register(ushort typeId, Func<ClientEntity> creator)
    {
        s_creators[typeId] = creator;
    }

    public static ClientEntity? Create(ushort typeId)
    {
        if (s_creators.TryGetValue(typeId, out var creator))
            return creator();
        return null;
    }
}
