using System;
using System.IO;
using System.Reflection;
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
            Atlas.Log.Warning(
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
