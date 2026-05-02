using System;
using System.Collections.Generic;
using System.Threading;

namespace Atlas.Coro;

public readonly partial struct AtlasTask
{
    public static AtlasTask<(T1, T2)> WhenAll<T1, T2>(AtlasTask<T1> t1, AtlasTask<T2> t2)
    {
        if (t1.Status != AtlasTaskStatus.Pending && t2.Status != AtlasTaskStatus.Pending)
        {
            try
            {
                var r1 = t1.GetAwaiter().GetResult();
                var r2 = t2.GetAwaiter().GetResult();
                return new AtlasTask<(T1, T2)>((r1, r2));
            }
            catch (Exception ex) { return AtlasTask<(T1, T2)>.FromException(ex); }
        }
        var src = new WhenAllSource2<T1, T2>(t1, t2);
        return new AtlasTask<(T1, T2)>(src, src.Version);
    }

    public static AtlasTask<(T1, T2, T3)> WhenAll<T1, T2, T3>(
        AtlasTask<T1> t1, AtlasTask<T2> t2, AtlasTask<T3> t3)
    {
        if (t1.Status != AtlasTaskStatus.Pending &&
            t2.Status != AtlasTaskStatus.Pending &&
            t3.Status != AtlasTaskStatus.Pending)
        {
            try
            {
                var r1 = t1.GetAwaiter().GetResult();
                var r2 = t2.GetAwaiter().GetResult();
                var r3 = t3.GetAwaiter().GetResult();
                return new AtlasTask<(T1, T2, T3)>((r1, r2, r3));
            }
            catch (Exception ex) { return AtlasTask<(T1, T2, T3)>.FromException(ex); }
        }
        var src = new WhenAllSource3<T1, T2, T3>(t1, t2, t3);
        return new AtlasTask<(T1, T2, T3)>(src, src.Version);
    }

    public static AtlasTask<T[]> WhenAll<T>(IEnumerable<AtlasTask<T>> tasks)
    {
        if (tasks is null) throw new ArgumentNullException(nameof(tasks));
        var arr = tasks as AtlasTask<T>[] ?? new List<AtlasTask<T>>(tasks).ToArray();
        if (arr.Length == 0) return new AtlasTask<T[]>(Array.Empty<T>());

        var allSync = true;
        for (var i = 0; i < arr.Length; i++)
        {
            if (arr[i].Status == AtlasTaskStatus.Pending) { allSync = false; break; }
        }
        if (allSync)
        {
            var results = new T[arr.Length];
            for (var i = 0; i < arr.Length; i++)
            {
                try { results[i] = arr[i].GetAwaiter().GetResult(); }
                catch (Exception ex) { return AtlasTask<T[]>.FromException(ex); }
            }
            return new AtlasTask<T[]>(results);
        }

        var src = new WhenAllSourceArray<T>(arr);
        return new AtlasTask<T[]>(src, src.Version);
    }

    public static AtlasTask WhenAll(AtlasTask t1, AtlasTask t2)
    {
        if (t1.Status != AtlasTaskStatus.Pending && t2.Status != AtlasTaskStatus.Pending)
        {
            try { t1.GetAwaiter().GetResult(); t2.GetAwaiter().GetResult(); return CompletedTask; }
            catch (Exception ex) { return FromException(ex); }
        }
        var src = new WhenAllSourceVoid2(t1, t2);
        return new AtlasTask(src, src.Version);
    }
}

internal sealed class WhenAllSource2<T1, T2> : IAtlasTaskSource<(T1, T2)>
{
    private AtlasTaskCompletionSourceCore<(T1, T2)> _core;
    private T1 _r1 = default!;
    private T2 _r2 = default!;
    private int _remaining;

    public WhenAllSource2(AtlasTask<T1> t1, AtlasTask<T2> t2)
    {
        _remaining = 2;
        var a1 = t1.GetAwaiter();
        if (a1.IsCompleted) Slot1(a1);
        else a1.UnsafeOnCompleted(() => Slot1(a1));

        var a2 = t2.GetAwaiter();
        if (a2.IsCompleted) Slot2(a2);
        else a2.UnsafeOnCompleted(() => Slot2(a2));
    }

    private void Slot1(AtlasTask<T1>.Awaiter a)
    {
        try { _r1 = a.GetResult(); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        if (Interlocked.Decrement(ref _remaining) == 0) _core.TrySetResult((_r1, _r2));
    }

    private void Slot2(AtlasTask<T2>.Awaiter a)
    {
        try { _r2 = a.GetResult(); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        if (Interlocked.Decrement(ref _remaining) == 0) _core.TrySetResult((_r1, _r2));
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
    public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
    public (T1, T2) GetResult(short t) => _core.GetResult(t);
}

internal sealed class WhenAllSource3<T1, T2, T3> : IAtlasTaskSource<(T1, T2, T3)>
{
    private AtlasTaskCompletionSourceCore<(T1, T2, T3)> _core;
    private T1 _r1 = default!;
    private T2 _r2 = default!;
    private T3 _r3 = default!;
    private int _remaining;

    public WhenAllSource3(AtlasTask<T1> t1, AtlasTask<T2> t2, AtlasTask<T3> t3)
    {
        _remaining = 3;
        var a1 = t1.GetAwaiter();
        if (a1.IsCompleted) Slot1(a1); else a1.UnsafeOnCompleted(() => Slot1(a1));
        var a2 = t2.GetAwaiter();
        if (a2.IsCompleted) Slot2(a2); else a2.UnsafeOnCompleted(() => Slot2(a2));
        var a3 = t3.GetAwaiter();
        if (a3.IsCompleted) Slot3(a3); else a3.UnsafeOnCompleted(() => Slot3(a3));
    }

    private void Slot1(AtlasTask<T1>.Awaiter a)
    {
        try { _r1 = a.GetResult(); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        if (Interlocked.Decrement(ref _remaining) == 0) _core.TrySetResult((_r1, _r2, _r3));
    }
    private void Slot2(AtlasTask<T2>.Awaiter a)
    {
        try { _r2 = a.GetResult(); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        if (Interlocked.Decrement(ref _remaining) == 0) _core.TrySetResult((_r1, _r2, _r3));
    }
    private void Slot3(AtlasTask<T3>.Awaiter a)
    {
        try { _r3 = a.GetResult(); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        if (Interlocked.Decrement(ref _remaining) == 0) _core.TrySetResult((_r1, _r2, _r3));
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
    public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
    public (T1, T2, T3) GetResult(short t) => _core.GetResult(t);
}

internal sealed class WhenAllSourceArray<T> : IAtlasTaskSource<T[]>
{
    private AtlasTaskCompletionSourceCore<T[]> _core;
    private readonly T[] _results;
    private int _remaining;

    public WhenAllSourceArray(AtlasTask<T>[] tasks)
    {
        _results = new T[tasks.Length];
        _remaining = tasks.Length;
        for (var i = 0; i < tasks.Length; i++)
        {
            var idx = i;
            var awaiter = tasks[i].GetAwaiter();
            if (awaiter.IsCompleted) Handle(awaiter, idx);
            else awaiter.UnsafeOnCompleted(() => Handle(awaiter, idx));
        }
    }

    private void Handle(AtlasTask<T>.Awaiter awaiter, int idx)
    {
        try { _results[idx] = awaiter.GetResult(); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        if (Interlocked.Decrement(ref _remaining) == 0) _core.TrySetResult(_results);
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
    public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
    public T[] GetResult(short t) => _core.GetResult(t);
}

internal sealed class WhenAllSourceVoid2 : IAtlasTaskSource<AtlasUnit>
{
    private AtlasTaskCompletionSourceCore<AtlasUnit> _core;
    private int _remaining;

    public WhenAllSourceVoid2(AtlasTask t1, AtlasTask t2)
    {
        _remaining = 2;
        var a1 = t1.GetAwaiter();
        if (a1.IsCompleted) Slot(a1); else a1.UnsafeOnCompleted(() => Slot(a1));
        var a2 = t2.GetAwaiter();
        if (a2.IsCompleted) Slot(a2); else a2.UnsafeOnCompleted(() => Slot(a2));
    }

    private void Slot(AtlasTask.Awaiter a)
    {
        try { a.GetResult(); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        if (Interlocked.Decrement(ref _remaining) == 0) _core.TrySetResult(AtlasUnit.Default);
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
    public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
    public AtlasUnit GetResult(short t) => _core.GetResult(t);
}
