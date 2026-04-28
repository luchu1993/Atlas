using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading;
using Atlas.Diagnostics;
using Atlas.Entity;
using Atlas.Runtime.Diagnostics;

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

        // Install the Tracy backend before any script user code runs, so
        // the first OnTick already produces zones — and so any module
        // initializer that opens a zone during assembly load lands in the
        // trace instead of falling silently into the null backend.
        // SetBackend is idempotent on a fresh runtime: a re-init after
        // hot-reload reaches this with the null backend re-established by
        // DoEngineShutdown's ResetBackend, so the install succeeds again.
        if (!Profiler.SetBackend(new TracyProfilerBackend()))
        {
            Log.Warning("Tracy profiler backend already installed — skipping re-install");
        }

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
        // Drop the Tracy backend reference so a subsequent EngineInit (e.g.
        // hot reload) can re-install cleanly. The cached frame/plot name
        // pointers leak by design — Tracy keys streams by pointer
        // identity and freeing would corrupt the trace history of any
        // already-connected viewer.
        Profiler.ResetBackend();
    }

    internal static void DoOnInit(bool isReload)
    {
        EntityManager.Instance.OnInitAll(isReload);
    }

    internal static void DoOnTick(float deltaTime)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ScriptOnTick);
        using (Profiler.ZoneN(ProfilerNames.ScriptSyncContextFlush))
            EngineContext.SyncContext?.ProcessQueue();
        using (Profiler.ZoneN(ProfilerNames.ScriptEntityTickAll))
            EntityManager.Instance.OnTickAll(deltaTime);
        // Collect property/volatile dirty bits that OnTick may have set and
        // forward them to the cell layer before witnesses sweep this tick's
        // updates. Skipping this step leaves event_seq pinned at 0 on the
        // C++ side and no property delta ever reaches a client.
        PublishReplicationAllWithGCProbe();
    }

    // Wraps PublishReplicationAll with a lightweight GC collection detector.
    // Snapshots GC generation counts before and after the call; logs a warning
    // when a collection occurred, giving correlation data to confirm whether
    // the observed max=6.59ms spike in Script.PublishReplicationAll is GC-induced
    // rather than algorithmic. Zero overhead on the happy path (two int reads).
    private static void PublishReplicationAllWithGCProbe()
    {
        var g0Before = GC.CollectionCount(0);
        var g1Before = GC.CollectionCount(1);
        var g2Before = GC.CollectionCount(2);
        var sw = Stopwatch.GetTimestamp();

        using (Profiler.ZoneN(ProfilerNames.ScriptPublishReplicationAll))
            EntityManager.Instance.PublishReplicationAll();

        var elapsedUs = (Stopwatch.GetTimestamp() - sw) * 1_000_000L / Stopwatch.Frequency;
        var g0Delta = GC.CollectionCount(0) - g0Before;
        var g1Delta = GC.CollectionCount(1) - g1Before;
        var g2Delta = GC.CollectionCount(2) - g2Before;

        if (g0Delta > 0 || g1Delta > 0 || g2Delta > 0)
        {
            Log.Warning(
                $"[GC-in-tick] PublishReplicationAll took {elapsedUs} µs; " +
                $"GC fired: gen0+{g0Delta} gen1+{g1Delta} gen2+{g2Delta}. " +
                "Possible GC pressure from replication codegen.");
        }
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
