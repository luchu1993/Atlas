using System;
using System.Runtime.InteropServices;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Coro.Rpc;
using Atlas.Core;

namespace Atlas.Client;

// Bound by name from ClrScriptEngine when client_app sets lifecycle_type =
// "Atlas.Client.DesktopLifecycle". OnTick drains the coroutine loop.
public static class DesktopLifecycle
{
    private static ManagedAtlasLoop? _coroLoop;
    private static ManagedRpcRegistry? _rpcRegistry;

    [UnmanagedCallersOnly]
    public static int EngineInit()
    {
        try
        {
            _coroLoop = new ManagedAtlasLoop();
            _coroLoop.UnhandledException += static ex =>
                Console.Error.WriteLine($"AtlasTask continuation threw: {ex.Message}");
            AtlasLoop.Install(_coroLoop);
            _rpcRegistry = new ManagedRpcRegistry(_coroLoop);
            AtlasRpcRegistryHost.Install(_rpcRegistry);
            AtlasShutdownToken.Reset();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int EngineShutdown()
    {
        try
        {
            AtlasShutdownToken.RequestShutdown();
            AtlasRpcRegistryHost.Reset();
            _rpcRegistry?.Dispose();
            _rpcRegistry = null;
            _coroLoop?.Dispose();
            _coroLoop = null;
            AtlasLoop.Reset();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnInit(byte _) => 0;

    [UnmanagedCallersOnly]
    public static int OnTick(float deltaTime)
    {
        try
        {
            _coroLoop?.Drain();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnShutdown() => 0;
}
