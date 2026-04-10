using System;
using System.Runtime.InteropServices;
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
/// </remarks>
internal static unsafe class Lifecycle
{
    [UnmanagedCallersOnly]
    public static int EngineInit()
    {
        try
        {
            EngineContext.Initialize();
            Log.Info("Atlas C# runtime initialized");
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
            Log.Info("Atlas C# runtime shutting down");
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
}
