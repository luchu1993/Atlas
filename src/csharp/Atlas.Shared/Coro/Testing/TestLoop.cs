using System;
using System.Collections.Generic;
using System.Threading;

namespace Atlas.Coro.Testing;

// Deterministic IAtlasLoop for unit tests — time and frames only advance
// when the test asks them to.
public sealed class TestLoop : IAtlasLoop
{
    private readonly List<PendingCallback> _nextFrame = new();
    private readonly List<PendingTimer> _timers = new();
    private long _currentFrame;
    private long _nextTimerHandle = 1;
    private long _currentTimeMs;

    public bool IsMainThread => true;
    public long CurrentFrame => _currentFrame;
    public int PendingFrameCallbacks => _nextFrame.Count;
    public int PendingTimers => _timers.Count;

    public void PostNextFrame(Action<object?> cb, object? state)
    {
        lock (_nextFrame) _nextFrame.Add(new PendingCallback(cb, state));
    }

    public long RegisterTimer(int milliseconds, Action<object?> cb, object? state)
    {
        var handle = _nextTimerHandle++;
        _timers.Add(new PendingTimer(handle, _currentTimeMs + milliseconds, cb, state));
        return handle;
    }

    public void CancelTimer(long handle)
    {
        for (var i = 0; i < _timers.Count; i++)
        {
            if (_timers[i].Handle == handle) { _timers.RemoveAt(i); return; }
        }
    }

    public void PostMainThread(Action<object?> cb, object? state)
    {
        lock (_nextFrame) _nextFrame.Add(new PendingCallback(cb, state));
    }

    // Drains the snapshot queued before this call; re-posts go to next frame.
    public int RunNextFrame()
    {
        _currentFrame++;
        PendingCallback[] snapshot;
        int count;
        lock (_nextFrame)
        {
            count = _nextFrame.Count;
            if (count == 0) return 0;
            snapshot = new PendingCallback[count];
            _nextFrame.CopyTo(0, snapshot, 0, count);
            _nextFrame.RemoveRange(0, count);
        }
        for (var i = 0; i < count; i++) snapshot[i].Callback(snapshot[i].State);
        return count;
    }

    public int AdvanceTime(int milliseconds)
    {
        if (milliseconds < 0) throw new ArgumentOutOfRangeException(nameof(milliseconds));
        _currentTimeMs += milliseconds;
        return DrainDueTimers();
    }

    private int DrainDueTimers()
    {
        var fired = 0;
        for (var i = 0; i < _timers.Count;)
        {
            if (_timers[i].DueMs <= _currentTimeMs)
            {
                var t = _timers[i];
                _timers.RemoveAt(i);
                t.Callback(t.State);
                fired++;
            }
            else { i++; }
        }
        return fired;
    }

    public T RunAwait<T>(AtlasTask<T> task, int maxFrames = 1000, int frameMs = 16)
    {
        var awaiter = task.GetAwaiter();
        for (var i = 0; i < maxFrames; i++)
        {
            if (awaiter.IsCompleted) return awaiter.GetResult();
            var didWork = RunNextFrame() > 0;
            if (AdvanceTime(frameMs) > 0) didWork = true;
            if (!didWork) Thread.Sleep(1);
        }
        if (awaiter.IsCompleted) return awaiter.GetResult();
        throw new TimeoutException("TestLoop.RunAwait: task did not complete within budget");
    }

    public void RunAwait(AtlasTask task, int maxFrames = 1000, int frameMs = 16)
    {
        var awaiter = task.GetAwaiter();
        for (var i = 0; i < maxFrames; i++)
        {
            if (awaiter.IsCompleted) { awaiter.GetResult(); return; }
            var didWork = RunNextFrame() > 0;
            if (AdvanceTime(frameMs) > 0) didWork = true;
            if (!didWork) Thread.Sleep(1);
        }
        if (awaiter.IsCompleted) { awaiter.GetResult(); return; }
        throw new TimeoutException("TestLoop.RunAwait: task did not complete within budget");
    }

    private readonly struct PendingCallback
    {
        public readonly Action<object?> Callback;
        public readonly object? State;
        public PendingCallback(Action<object?> cb, object? s) { Callback = cb; State = s; }
    }

    private readonly struct PendingTimer
    {
        public readonly long Handle;
        public readonly long DueMs;
        public readonly Action<object?> Callback;
        public readonly object? State;
        public PendingTimer(long h, long due, Action<object?> cb, object? s)
        { Handle = h; DueMs = due; Callback = cb; State = s; }
    }
}
