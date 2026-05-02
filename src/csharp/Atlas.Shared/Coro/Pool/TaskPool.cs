using System.Collections.Generic;

namespace Atlas.Coro;

// Thread-local stack pool — lock-free under the single-threaded coro model.
internal static class TaskPool<T> where T : class
{
    [System.ThreadStatic]
    private static Stack<T>? _pool;

    public const int MaxSize = 256;

    public static bool TryRent(out T? item)
    {
        var pool = _pool;
        if (pool is { Count: > 0 })
        {
            item = pool.Pop();
            return true;
        }
        item = null;
        return false;
    }

    public static void Return(T item)
    {
        var pool = _pool ??= new Stack<T>(16);
        if (pool.Count >= MaxSize) return;
        pool.Push(item);
    }
}
