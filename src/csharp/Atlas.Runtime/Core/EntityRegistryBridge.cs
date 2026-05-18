using System;

namespace Atlas.Core;

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

    public static void RegisterComponent(ReadOnlySpan<byte> data)
    {
        NativeApi.RegisterComponent(data);
    }

    public static void SetEntityDefDigest(ReadOnlySpan<byte> data)
    {
        NativeApi.SetEntityDefDigest(data);
    }
}
