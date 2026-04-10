using System;
using System.Runtime.InteropServices;
using Atlas.Core;
using Atlas.Entity;

namespace Atlas.RuntimeTest;

// ============================================================================
// LifecycleTestEntryPoint — forwarding [UnmanagedCallersOnly] entry points
// ============================================================================
//
// These methods forward to Lifecycle / EntityManager / Log / Time classes
// in Atlas.Runtime.  They must live in Atlas.RuntimeTest to avoid the
// dual-Assembly-instance issue (see implementation_notes.md §2).

public static class LifecycleTestEntryPoint
{
    // ---- Lifecycle forwarding ------------------------------------------------

    [UnmanagedCallersOnly]
    public static int EngineInit()
    {
        try
        {
            EngineContext.Initialize();
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
            EntityManager.Instance.OnShutdownAll();
            EngineContext.Shutdown();
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
            EntityManager.Instance.OnInitAll(isReload != 0);
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
            EntityManager.Instance.OnTickAll(deltaTime);
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
            EntityManager.Instance.OnShutdownAll();
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // ---- Log / Time verification --------------------------------------------

    [UnmanagedCallersOnly]
    public static unsafe int LogInfoTest(byte* utf8, int len)
    {
        try
        {
            var msg = System.Text.Encoding.UTF8.GetString(utf8, len);
            Atlas.Log.Info(msg);
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static unsafe int GetDeltaTime(float* outDt)
    {
        try
        {
            *outDt = Atlas.Time.DeltaTime;
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    // ---- Entity count query -------------------------------------------------

    [UnmanagedCallersOnly]
    public static unsafe int GetEntityCount(int* outCount)
    {
        try
        {
            *outCount = EntityManager.Instance.Count;
            return 0;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
            return -1;
        }
    }
}
