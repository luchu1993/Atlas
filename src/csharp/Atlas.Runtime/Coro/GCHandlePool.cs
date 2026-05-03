using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Atlas.Runtime.Coro;

// Reuses GCHandle slots across RPCs. gch.Target = null releases the GC
// root but keeps the slot, so re-binding skips the per-call alloc cost.
internal static class GCHandlePool
{
    [ThreadStatic] private static Stack<GCHandle>? s_pool;

    private const int MaxPoolSize = 256;

    public static IntPtr Rent(object target)
    {
        var pool = s_pool;
        if (pool is { Count: > 0 })
        {
            var gch = pool.Pop();
            gch.Target = target;
            return GCHandle.ToIntPtr(gch);
        }
        return GCHandle.ToIntPtr(GCHandle.Alloc(target, GCHandleType.Normal));
    }

    public static void Return(IntPtr handle)
    {
        if (handle == IntPtr.Zero) return;
        var gch = GCHandle.FromIntPtr(handle);
        gch.Target = null;
        var pool = s_pool ??= new Stack<GCHandle>(64);
        if (pool.Count < MaxPoolSize) pool.Push(gch);
        else gch.Free();
    }
}
