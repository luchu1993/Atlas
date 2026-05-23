using System;

namespace Atlas.Client;

public static class ClientEntityFactory
{
    public static ClientEntityFactoryRegistry DefaultRegistry =>
        ClientCallbacks.DefaultSession.EntityFactory;

    public static void Register(ushort typeId, Func<ClientEntity> creator)
    {
        DefaultRegistry.Register(typeId, creator);
    }

    public static ClientEntity? Create(ushort typeId)
    {
        return DefaultRegistry.Create(typeId);
    }
}
