using System;

namespace Atlas.Client;

public static class ClientEntityRegistryBridge
{
    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        ClientHost.RegisterEntityType(data);
    }

    public static void RegisterStruct(ReadOnlySpan<byte> data)
    {
        ClientHost.RegisterStruct(data);
    }

    public static void SetEntityDefDigest(ReadOnlySpan<byte> data)
    {
        ClientHost.SetEntityDefDigest(data);
    }
}
