using System;
using Atlas.Entity;

namespace Atlas.Core;

/// <summary>
/// Global engine context managing initialization state and lifecycle coordination.
/// Set up by <see cref="Lifecycle"/> entry points, which are called from C++ ClrScriptEngine.
/// </summary>
internal static class EngineContext
{
    private static bool _initialized;

    public static void Initialize()
    {
        if (_initialized)
            throw new InvalidOperationException("EngineContext already initialized");

        var prefix = NativeApi.GetProcessPrefix();
        EntityManager.Instance.SetProcessPrefix(prefix);

        _initialized = true;
    }

    public static bool IsInitialized => _initialized;

    public static void Shutdown()
    {
        if (!_initialized) return;
        _initialized = false;
    }
}
