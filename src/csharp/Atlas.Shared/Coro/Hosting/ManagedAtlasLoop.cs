using System;
using System.Collections.Concurrent;
using System.Threading;

namespace Atlas.Coro.Hosting;

// IAtlasLoop backed by .NET ThreadPool timer + an in-process queue. Used by
// server (Atlas.Runtime) and desktop client. UnityLoop replaces it on Unity.
public sealed class ManagedAtlasLoop : IAtlasLoop, IDisposable
{
    private readonly ConcurrentQueue<PendingCallback> _mainQueue = new();
    private readonly ConcurrentDictionary<long, TimerEntry> _timers = new();
    private long _nextTimerId;
    private long _currentFrame;
    private readonly int _mainThreadId;
    private bool _disposed;

    // Hosts wanting logging install their own handler (e.g. Atlas.Log.Error).
    public event Action<Exception>? UnhandledException;

    public ManagedAtlasLoop()
    {
        _mainThreadId = Environment.CurrentManagedThreadId;
    }

    public bool IsMainThread => Environment.CurrentManagedThreadId == _mainThreadId;
    public long CurrentFrame => Interlocked.Read(ref _currentFrame);

    public int PendingCount => _mainQueue.Count;
    public int ActiveTimers => _timers.Count;

    public void PostNextFrame(Action<object?> cb, object? state)
    {
        if (cb is null) throw new ArgumentNullException(nameof(cb));
        _mainQueue.Enqueue(new PendingCallback(cb, state));
    }

    public void PostMainThread(Action<object?> cb, object? state)
    {
        if (cb is null) throw new ArgumentNullException(nameof(cb));
        _mainQueue.Enqueue(new PendingCallback(cb, state));
    }

    public long RegisterTimer(int milliseconds, Action<object?> cb, object? state)
    {
        if (cb is null) throw new ArgumentNullException(nameof(cb));
        if (milliseconds < 0) throw new ArgumentOutOfRangeException(nameof(milliseconds));

        var id = Interlocked.Increment(ref _nextTimerId);
        var entry = new TimerEntry(this, id, cb, state);
        var timer = new Timer(TimerFiredCallback, entry, milliseconds, Timeout.Infinite);
        entry.Timer = timer;
        _timers[id] = entry;
        return id;
    }

    public void CancelTimer(long handle)
    {
        if (_timers.TryRemove(handle, out var entry))
        {
            entry.Cancelled = true;
            entry.Timer?.Dispose();
        }
    }

    // Hosts call this once per frame from the main thread.
    public int Drain()
    {
        Interlocked.Increment(ref _currentFrame);
        var processed = 0;
        while (_mainQueue.TryDequeue(out var item))
        {
            try { item.Callback(item.State); }
            catch (Exception ex) { UnhandledException?.Invoke(ex); }
            processed++;
        }
        return processed;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        foreach (var kv in _timers) kv.Value.Timer?.Dispose();
        _timers.Clear();
    }

    private static readonly Action<object?> MainSideFire = static state =>
    {
        var entry = (TimerEntry)state!;
        if (entry.Cancelled) return;
        if (entry.Loop._timers.TryRemove(entry.Id, out _))
            entry.Timer?.Dispose();
        entry.Callback(entry.State);
    };

    private static readonly TimerCallback TimerFiredCallback = static state =>
    {
        var entry = (TimerEntry)state!;
        if (entry.Cancelled) return;
        entry.Loop._mainQueue.Enqueue(new PendingCallback(MainSideFire, entry));
    };

    private readonly struct PendingCallback
    {
        public readonly Action<object?> Callback;
        public readonly object? State;
        public PendingCallback(Action<object?> cb, object? s) { Callback = cb; State = s; }
    }

    private sealed class TimerEntry
    {
        public readonly ManagedAtlasLoop Loop;
        public readonly long Id;
        public readonly Action<object?> Callback;
        public readonly object? State;
        public Timer? Timer;
        public bool Cancelled;

        public TimerEntry(ManagedAtlasLoop loop, long id, Action<object?> cb, object? s)
        { Loop = loop; Id = id; Callback = cb; State = s; }
    }
}
