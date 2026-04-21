using System;

namespace Atlas.Client;

/// <summary>
/// Public bridge for generated entity-type registration code on the client
/// side. The generator emits a call into this class from a
/// <c>[ModuleInitializer]</c>; the actual native registration call is
/// resolved at runtime through <see cref="ClientHost.RegisterEntityTypeHandler"/>
/// so the same generator output runs against either the desktop CoreCLR
/// host (atlas_client.exe via Atlas.Client.Desktop) or the Unity IL2CPP /
/// Mono host (atlas_net_client via the Unity package).
/// </summary>
public static class ClientEntityRegistryBridge
{
    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        ClientHost.RegisterEntityType(data);
    }
}
