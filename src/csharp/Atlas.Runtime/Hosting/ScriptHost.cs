using System;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Atlas.Hosting;

/// <summary>
/// Manages a single script assembly's lifecycle within a collectible AssemblyLoadContext.
/// Supports load, unload, and GC verification for hot-reload.
/// </summary>
internal sealed class ScriptHost : IDisposable
{
    private ScriptLoadContext? _context;
    private WeakReference? _contextRef;
    private Assembly? _scriptAssembly;

    public bool IsLoaded => _context != null;

    public Assembly? ScriptAssembly => _scriptAssembly;

    public void Load(string assemblyPath)
    {
        if (_context != null)
            throw new InvalidOperationException("ScriptHost already has a loaded context. Unload first.");

        var dir = Path.GetDirectoryName(Path.GetFullPath(assemblyPath))!;
        _context = new ScriptLoadContext(dir);
        _scriptAssembly = _context.LoadFromAssemblyPath(Path.GetFullPath(assemblyPath));
        _contextRef = new WeakReference(_context);

        // Force every module initializer in the loaded assembly to run.
        // `LoadFromAssemblyPath` does NOT execute module initializers — they
        // only fire the first time the CLR *uses* a type from the module,
        // which with collectible ALCs means "never" if the host never
        // accesses one. DefGenerator emits DefEntityTypeRegistry.RegisterAll
        // as a [ModuleInitializer]; without this kick it silently never
        // registers any entity types with the C++ engine, so subsequent
        // RPC dispatch fails with "non-exposed base method".
        foreach (var module in _scriptAssembly.GetModules())
        {
            try { RuntimeHelpers.RunModuleConstructor(module.ModuleHandle); }
            catch (Exception ex)
            {
                // Unwrap TypeInitializationException chains to surface the
                // actual inner exception type and message. The generator
                // swallows DllNotFoundException and InvalidOperationException
                // internally, so anything making it out here is a real
                // structural failure (missing engine assembly, missing type,
                // etc.) that should fail the load loudly.
                var root = ex;
                while (root.InnerException != null) root = root.InnerException;
                Atlas.Diagnostics.Log.Error(
                    $"ScriptHost.Load: module init '{module.Name}' failed. "
                    + $"Outer={ex.GetType().Name}: {ex.Message}. "
                    + $"Root={root.GetType().Name}: {root.Message}");
                throw;
            }
        }
    }

    /// <summary>
    /// Unload the current context and wait for GC to collect it.
    /// Returns true if collected within timeout, false if a leak is suspected.
    /// </summary>
    public bool Unload(TimeSpan timeout)
    {
        if (_context == null) return true;

        _scriptAssembly = null;
        _context.Unload();
        _context = null;

        var deadline = DateTime.UtcNow + timeout;
        while (_contextRef!.IsAlive && DateTime.UtcNow < deadline)
        {
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            Thread.Sleep(100);
        }

        if (_contextRef.IsAlive)
        {
            Atlas.Diagnostics.Log.Warning(
                "ScriptLoadContext was not collected within timeout — possible GCHandle leak");
            return false;
        }

        return true;
    }

    public void Dispose()
    {
        Unload(TimeSpan.FromSeconds(5));
    }
}
