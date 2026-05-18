using System;

namespace Atlas;

public static class AppEvents
{
    // Fires once on the first cellapp/baseapp tick after the script runtime
    // is ready. Subscribe from a [ModuleInitializer] for at-boot work.
    public static event Action? AppInit;

    // Handler-side opt-in to retry on the next tick when a prerequisite
    // (e.g. entity-ID pool) is not yet ready.
    public static void DeferAppInit() => s_appInitDeferred = true;

    private static bool s_appInitFired;
    private static bool s_appInitDeferred;

    internal static void TryFireAppInit()
    {
        if (s_appInitFired) return;
        s_appInitDeferred = false;
        AppInit?.Invoke();
        if (!s_appInitDeferred) s_appInitFired = true;
    }
}
