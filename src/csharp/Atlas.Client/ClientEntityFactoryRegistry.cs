using System;
using System.Collections.Generic;

namespace Atlas.Client;

public sealed class ClientEntityFactoryRegistry
{
    private readonly Dictionary<ushort, Func<ClientEntity>> _creators = new();

    public void Register(ushort typeId, Func<ClientEntity> creator)
    {
        _creators[typeId] = creator ?? throw new ArgumentNullException(nameof(creator));
    }

    public ClientEntity? Create(ushort typeId)
    {
        return _creators.TryGetValue(typeId, out var creator) ? creator() : null;
    }

    public void ClearForTest()
    {
        _creators.Clear();
    }
}
