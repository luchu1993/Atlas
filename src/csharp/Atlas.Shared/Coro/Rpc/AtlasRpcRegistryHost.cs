using System;

namespace Atlas.Coro.Rpc;

// Process-wide IAtlasRpcRegistry slot. Server installs NativeRpcRegistry,
// client installs ManagedRpcRegistry, tests install their own.
public static class AtlasRpcRegistryHost
{
    private static IAtlasRpcRegistry? _current;

    public static IAtlasRpcRegistry Current =>
        _current ?? throw new InvalidOperationException(
            "AtlasRpcRegistryHost not installed — call Install() during host bootstrap");

    public static bool IsInstalled => _current is not null;

    public static void Install(IAtlasRpcRegistry registry)
    {
        if (registry is null) throw new ArgumentNullException(nameof(registry));
        _current = registry;
    }

    public static void Reset() => _current = null;
}
