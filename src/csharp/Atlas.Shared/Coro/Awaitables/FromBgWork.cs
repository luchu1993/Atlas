using System;
using System.Threading;

namespace Atlas.Coro;

public readonly partial struct AtlasTask
{
    public static AtlasTask<T> FromBgWork<T>(Func<T> work)
    {
        if (work is null) throw new ArgumentNullException(nameof(work));
        var src = new BgWorkSource<T>();
        src.Schedule(work);
        return new AtlasTask<T>(src, src.Version);
    }

    public static AtlasTask FromBgWork(Action work)
    {
        if (work is null) throw new ArgumentNullException(nameof(work));
        var src = new BgWorkSource<AtlasUnit>();
        src.Schedule(() => { work(); return AtlasUnit.Default; });
        return new AtlasTask(src, src.Version);
    }
}

internal sealed class BgWorkSource<T> : IAtlasTaskSource<T>
{
    private AtlasTaskCompletionSourceCore<T> _core;
    private Func<T>? _work;
    private T _result = default!;
    private Exception? _error;

    private static readonly Action<object?> OnFinishOnMain = static state =>
    {
        var self = (BgWorkSource<T>)state!;
        if (self._error is not null) self._core.TrySetException(self._error);
        else self._core.TrySetResult(self._result);
    };

    private static readonly WaitCallback RunCallback =
        static state => ((BgWorkSource<T>)state!).Run();

    public void Schedule(Func<T> work)
    {
        _work = work;
        ThreadPool.UnsafeQueueUserWorkItem(RunCallback, this);
    }

    private void Run()
    {
        try { _result = _work!(); }
        catch (Exception ex) { _error = ex; }
        AtlasLoop.Current.PostMainThread(OnFinishOnMain, this);
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short t) => _core.GetStatus(t);
    public void OnCompleted(Action<object?> c, object? s, short t) => _core.OnCompleted(c, s, t);
    public T GetResult(short t) => _core.GetResult(t);
}
