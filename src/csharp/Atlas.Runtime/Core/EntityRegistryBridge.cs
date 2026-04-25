using System;

namespace Atlas.Core;

/// <summary>
/// Public bridge for generated entity type registration code.
/// Generated DefEntityTypeRegistry calls this to forward binary descriptors
/// to the C++ EntityDefRegistry via NativeApi.
/// </summary>
public static class EntityRegistryBridge
{
    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        NativeApi.RegisterEntityType(data);
    }

    public static void RegisterStruct(ReadOnlySpan<byte> data)
    {
        NativeApi.RegisterStruct(data);
    }
}
