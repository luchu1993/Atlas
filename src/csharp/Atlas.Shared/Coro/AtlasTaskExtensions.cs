using System;

namespace Atlas.Coro;

public static class AtlasTaskExtensions
{
    // Drains the task's result so the source returns to pool. Exceptions go
    // through UnhandledException — never silently swallowed.
    public static void Forget(this AtlasTask task)
    {
        var awaiter = task.GetAwaiter();
        if (awaiter.IsCompleted) { TryConsume(awaiter); return; }
        awaiter.UnsafeOnCompleted(() => TryConsume(awaiter));
    }

    public static void Forget<T>(this AtlasTask<T> task)
    {
        var awaiter = task.GetAwaiter();
        if (awaiter.IsCompleted) { TryConsume(awaiter); return; }
        awaiter.UnsafeOnCompleted(() => TryConsume(awaiter));
    }

    public static event Action<Exception>? UnhandledException;

    private static void TryConsume(AtlasTask.Awaiter awaiter)
    {
        try { awaiter.GetResult(); }
        catch (Exception ex) { ReportUnhandled(ex); }
    }

    private static void TryConsume<T>(AtlasTask<T>.Awaiter awaiter)
    {
        try { awaiter.GetResult(); }
        catch (Exception ex) { ReportUnhandled(ex); }
    }

    private static void ReportUnhandled(Exception ex)
    {
        var handler = UnhandledException;
        if (handler is null) throw ex;
        handler(ex);
    }
}
