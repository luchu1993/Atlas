using System;
using System.Threading;

namespace Atlas.Coro;

public readonly partial struct AtlasTask
{
    public static AtlasTask<int> WhenAny(params AtlasTask[] tasks)
    {
        if (tasks is null) throw new ArgumentNullException(nameof(tasks));
        if (tasks.Length == 0) throw new ArgumentException("WhenAny requires at least one task");

        for (var i = 0; i < tasks.Length; i++)
        {
            if (tasks[i].Status == AtlasTaskStatus.Pending) continue;
            try { tasks[i].GetAwaiter().GetResult(); }
            catch (Exception ex)
            {
                DrainOthers(tasks, i);
                return AtlasTask<int>.FromException(ex);
            }
            DrainOthers(tasks, i);
            return new AtlasTask<int>(i);
        }

        var src = new WhenAnyVoidSource(tasks);
        return new AtlasTask<int>(src, src.Version);
    }

    public static AtlasTask<(int index, T result)> WhenAny<T>(params AtlasTask<T>[] tasks)
    {
        if (tasks is null) throw new ArgumentNullException(nameof(tasks));
        if (tasks.Length == 0) throw new ArgumentException("WhenAny requires at least one task");

        for (var i = 0; i < tasks.Length; i++)
        {
            if (tasks[i].Status == AtlasTaskStatus.Pending) continue;
            T value;
            try { value = tasks[i].GetAwaiter().GetResult(); }
            catch (Exception ex)
            {
                DrainOthers(tasks, i);
                return AtlasTask<(int, T)>.FromException(ex);
            }
            DrainOthers(tasks, i);
            return new AtlasTask<(int, T)>((i, value));
        }

        var src = new WhenAnySource<T>(tasks);
        return new AtlasTask<(int, T)>(src, src.Version);
    }

    // Losing tasks: errors are dropped; we just need GetResult to release pooled boxes.
    private static void DrainOthers(AtlasTask[] tasks, int skipIndex)
    {
        for (var i = 0; i < tasks.Length; i++)
        {
            if (i == skipIndex) continue;
            if (tasks[i].Status == AtlasTaskStatus.Pending) continue;
            try { tasks[i].GetAwaiter().GetResult(); }
            catch (Exception) { }
        }
    }

    private static void DrainOthers<T>(AtlasTask<T>[] tasks, int skipIndex)
    {
        for (var i = 0; i < tasks.Length; i++)
        {
            if (i == skipIndex) continue;
            if (tasks[i].Status == AtlasTaskStatus.Pending) continue;
            try { tasks[i].GetAwaiter().GetResult(); }
            catch (Exception) { }
        }
    }
}

internal sealed class WhenAnyVoidSource : IAtlasTaskSource<int>
{
    private AtlasTaskCompletionSourceCore<int> _core;
    private int _claimed;

    public WhenAnyVoidSource(AtlasTask[] tasks)
    {
        for (var i = 0; i < tasks.Length; i++)
        {
            var idx = i;
            var awaiter = tasks[i].GetAwaiter();
            if (awaiter.IsCompleted) { Handle(awaiter, idx); continue; }
            awaiter.UnsafeOnCompleted(() => Handle(awaiter, idx));
        }
    }

    private void Handle(AtlasTask.Awaiter awaiter, int idx)
    {
        var won = Interlocked.CompareExchange(ref _claimed, 1, 0) == 0;
        try
        {
            awaiter.GetResult();
            if (won) _core.TrySetResult(idx);
        }
        catch (Exception ex)
        {
            if (won) _core.TrySetException(ex);
        }
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
    public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
    public int GetResult(short t) => _core.GetResult(t);
}

internal sealed class WhenAnySource<T> : IAtlasTaskSource<(int, T)>
{
    private AtlasTaskCompletionSourceCore<(int, T)> _core;
    private int _claimed;

    public WhenAnySource(AtlasTask<T>[] tasks)
    {
        for (var i = 0; i < tasks.Length; i++)
        {
            var idx = i;
            var awaiter = tasks[i].GetAwaiter();
            if (awaiter.IsCompleted) { Handle(awaiter, idx); continue; }
            awaiter.UnsafeOnCompleted(() => Handle(awaiter, idx));
        }
    }

    private void Handle(AtlasTask<T>.Awaiter awaiter, int idx)
    {
        var won = Interlocked.CompareExchange(ref _claimed, 1, 0) == 0;
        try
        {
            var v = awaiter.GetResult();
            if (won) _core.TrySetResult((idx, v));
        }
        catch (Exception ex)
        {
            if (won) _core.TrySetException(ex);
        }
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
    public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
    public (int, T) GetResult(short t) => _core.GetResult(t);
}
