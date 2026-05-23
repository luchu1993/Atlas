using System;
using System.Collections.Generic;

namespace Atlas;

public static class AppEvents
{
    // Fires once on the first cellapp/baseapp tick after the script runtime is ready.
    public static event Action? AppInit;

    // Handler-side opt-in to retry on the next tick when a prerequisite
    // (e.g. entity-ID pool) is not yet ready.
    public static void DeferAppInit() => s_appInitDeferred = true;

    private static readonly List<IAtlasAppInitializer> s_appInitializers = new();
    private static bool s_appInitFired;
    private static bool s_appInitDeferred;

    internal static void RegisterScriptInitializer(IAtlasAppInitializer initializer)
    {
        ArgumentNullException.ThrowIfNull(initializer);
        if (s_appInitFired)
        {
            initializer.OnAppInit();
            return;
        }
        s_appInitializers.Add(initializer);
    }

    internal static void UnregisterScriptInitializer(IAtlasAppInitializer initializer)
    {
        s_appInitializers.Remove(initializer);
    }

    internal static void TryFireAppInit()
    {
        if (s_appInitFired) return;
        s_appInitDeferred = false;
        AppInit?.Invoke();
        foreach (var initializer in s_appInitializers.ToArray())
            initializer.OnAppInit();
        if (!s_appInitDeferred) s_appInitFired = true;
        if (s_appInitFired)
        {
            AppInit = null;
            s_appInitializers.Clear();
        }
    }

    internal static void Reset()
    {
        AppInit = null;
        s_appInitializers.Clear();
        s_appInitFired = false;
        s_appInitDeferred = false;
    }
}
