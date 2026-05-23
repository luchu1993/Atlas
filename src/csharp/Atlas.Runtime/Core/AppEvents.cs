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
        s_appInitializers.Add(initializer);
    }

    internal static void UnregisterScriptInitializer(IAtlasAppInitializer initializer)
    {
        for (int i = s_appInitializers.Count - 1; i >= 0; --i)
        {
            if (ReferenceEquals(s_appInitializers[i], initializer))
                s_appInitializers.RemoveAt(i);
        }
    }

    internal static void TryFireAppInit()
    {
        if (s_appInitFired && s_appInitializers.Count == 0) return;
        s_appInitDeferred = false;
        if (!s_appInitFired) AppInit?.Invoke();
        var initializers = s_appInitializers.ToArray();
        foreach (var initializer in initializers)
        {
            if (HasScriptInitializer(initializer))
                initializer.OnAppInit();
        }
        if (s_appInitDeferred) return;
        if (!s_appInitFired) s_appInitFired = true;
        AppInit = null;
        foreach (var initializer in initializers)
            UnregisterScriptInitializer(initializer);
    }

    internal static void Reset()
    {
        AppInit = null;
        s_appInitializers.Clear();
        s_appInitFired = false;
        s_appInitDeferred = false;
    }

    private static bool HasScriptInitializer(IAtlasAppInitializer initializer)
    {
        foreach (var current in s_appInitializers)
        {
            if (ReferenceEquals(current, initializer))
                return true;
        }
        return false;
    }
}
