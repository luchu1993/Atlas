using System;

namespace Atlas.Client;

/// <summary>
/// Public bridge for generated entity type registration code on the client side.
/// </summary>
public static class ClientEntityRegistryBridge
{
    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        ClientNativeApi.RegisterEntityType(data);
    }
}
