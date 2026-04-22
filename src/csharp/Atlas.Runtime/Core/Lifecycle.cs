using System;
using System.Runtime.InteropServices;
using System.Threading;
using Atlas.Entity;

namespace Atlas.Core;

/// <summary>
/// High-level lifecycle entry points called from C++ ClrScriptEngine.
/// Each method follows the try/catch + ErrorBridge pattern so exceptions
/// never escape into C++.
/// </summary>
/// <remarks>
/// <see cref="Bootstrap.Initialize"/> handles the low-level CLR setup
/// (error bridge, vtable). These methods handle the engine-level lifecycle.
///
/// Shutdown sequence (called by ClrScriptEngine):
///   1. OnShutdown()     — calls OnDestroy on all entities
///   2. EngineShutdown() — tears down EngineContext (does NOT touch entities)
/// </remarks>
internal static class Lifecycle
{
    // ---- Non-[UnmanagedCallersOnly] core logic (shared with test forwarders) ----

    internal static void DoEngineInit()
    {
        var syncContext = new AtlasSynchronizationContext();
        SynchronizationContext.SetSynchronizationContext(syncContext);
        EngineContext.SyncContext = syncContext;

        ThreadGuard.SetMainThread();

        EngineContext.Initialize();
        Log.Info("Atlas C# runtime initialized");
#if DEBUG
        Atlas.Diagnostics.GCMonitor.Start(TimeSpan.FromSeconds(60));
#endif
    }

    internal static void DoEngineShutdown()
    {
#if DEBUG
        Atlas.Diagnostics.GCMonitor.Stop();
#endif
        Log.Info("Atlas C# runtime shutting down");
        EngineContext.Shutdown();
    }

    internal static void DoOnInit(bool isReload)
    {
        EntityManager.Instance.OnInitAll(isReload);
    }

    internal static void DoOnTick(float deltaTime)
    {
        EngineContext.SyncContext?.ProcessQueue();
        EntityManager.Instance.OnTickAll(deltaTime);
        // Collect property/volatile dirty bits that OnTick may have set and
        // forward them to the cell layer before witnesses sweep this tick's
        // updates. Skipping this step leaves event_seq pinned at 0 on the
        // C++ side and no property delta ever reaches a client.
        EntityManager.Instance.PublishReplicationAll();
    }

    internal static void DoOnShutdown()
    {
        EntityManager.Instance.OnShutdownAll();
    }

    // ---- [UnmanagedCallersOnly] entry points (C++ calls these) ----

    [UnmanagedCallersOnly]
    public static int EngineInit()
    {
        try
        {
            DoEngineInit();
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
            DoEngineShutdown();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnInit(byte isReload)
    {
        try
        {
            DoOnInit(isReload != 0);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnTick(float deltaTime)
    {
        try
        {
            DoOnTick(deltaTime);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static int OnShutdown()
    {
        try
        {
            DoOnShutdown();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }
}
