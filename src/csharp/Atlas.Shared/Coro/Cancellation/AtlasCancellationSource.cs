using System;
using System.Collections.Generic;
using Atlas.Diagnostics;

namespace Atlas.Coro;

// Single-threaded cancellation source. Mirrors C++ atlas::CancellationSource.
// All state is touched on the main thread only — no locks, no Interlocked.
public sealed class AtlasCancellationSource : IDisposable
{
    private bool _cancelled;
    private long _nextId;
    private List<Entry>? _callbacks;
    private CancelRegistration _parentReg;
    private bool _disposed;

    public AtlasCancellationToken Token => new(this);

    public bool IsCancellationRequested => _cancelled;

    public void Cancel()
    {
        if (_cancelled || _disposed) return;
        _cancelled = true;

        if (_callbacks is null) return;
        var cbs = _callbacks;
        _callbacks = null;
        for (var i = 0; i < cbs.Count; i++)
        {
            // One source must drive all callbacks: keep going on failure
            // but surface the exception via Log so the bug isn't silent.
            try { cbs[i].Callback(cbs[i].State); }
            catch (Exception ex) { Log.Error($"AtlasCancellationSource callback threw: {ex}"); }
        }
    }

    // Creates a child source that is cancelled when this one cancels.
    // If this source is already cancelled, the returned child is born cancelled.
    public AtlasCancellationSource CreateLinked()
    {
        var child = new AtlasCancellationSource();
        if (_cancelled) { child.Cancel(); return child; }
        child._parentReg = Token.Register(static o => ((AtlasCancellationSource)o!).Cancel(), child);
        return child;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _parentReg.Dispose();
        _callbacks = null;
    }

    internal long RegisterCallback(Action<object?> cb, object? state)
    {
        if (_cancelled)
        {
            try { cb(state); }
            catch (Exception ex) { Log.Error($"AtlasCancellationSource callback threw: {ex}"); }
            return 0;
        }
        _callbacks ??= new List<Entry>(2);
        var id = ++_nextId;
        _callbacks.Add(new Entry(id, cb, state));
        return id;
    }

    internal void UnregisterCallback(long id)
    {
        if (_callbacks is null) return;
        for (var i = 0; i < _callbacks.Count; i++)
        {
            if (_callbacks[i].Id == id) { _callbacks.RemoveAt(i); return; }
        }
    }

    private readonly struct Entry
    {
        public readonly long Id;
        public readonly Action<object?> Callback;
        public readonly object? State;
        public Entry(long id, Action<object?> cb, object? s) { Id = id; Callback = cb; State = s; }
    }
}
