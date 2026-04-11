using System;
using System.Threading;
using Atlas.Entity;

namespace Atlas.Core;

/// <summary>
/// Global engine context managing initialization state and lifecycle coordination.
/// Set up by <see cref="Lifecycle"/> entry points, which are called from C++ ClrScriptEngine.
/// </summary>
/// <remarks>
/// <see cref="Initialize"/> calls <see cref="NativeApi.GetProcessPrefix"/> via
/// [LibraryImport("atlas_engine")]. This requires that:
///   1. atlas_engine.dll is loadable (same directory as the managed assembly).
///   2. An <c>INativeApiProvider</c> has been registered via
///      <c>atlas_set_native_api_provider()</c> before CLR bootstrap.
/// Both conditions are guaranteed by <c>ClrScriptEngine::initialize()</c>.
/// </remarks>
internal static class EngineContext
{
    private static bool _initialized;

    /// <summary>
    /// Custom SynchronizationContext installed during bootstrap.
    /// OnTick calls ProcessQueue() to run await continuations on the main thread.
    /// </summary>
    public static AtlasSynchronizationContext? SyncContext { get; set; }

    public static void Initialize()
    {
        if (_initialized)
            throw new InvalidOperationException("EngineContext already initialized");

        var prefix = NativeApi.GetProcessPrefix();
        EntityManager.Instance.SetProcessPrefix(prefix);
        EntityTypeRegistry.RegisterAll();
        NativeCallbacks.Register();

        _initialized = true;
    }

    public static bool IsInitialized => _initialized;

    public static void Shutdown()
    {
        if (!_initialized) return;
        NativeCallbacks.Reset();
        NativeApi.UnregisterAllEntityTypes();
        EntityManager.Instance.Reset();
        SyncContext = null;
        _initialized = false;
    }
}
