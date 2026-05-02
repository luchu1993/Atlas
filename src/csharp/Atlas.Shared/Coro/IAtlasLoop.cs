using System;

namespace Atlas.Coro;

// Platform abstraction over timer / next-frame / main-thread post.
public interface IAtlasLoop
{
    // Enqueue a callback for the next tick / frame on the main thread.
    void PostNextFrame(Action<object?> cb, object? state);

    // Schedule a timer; returns an opaque handle (>0). 0 is reserved for "invalid".
    long RegisterTimer(int milliseconds, Action<object?> cb, object? state);

    void CancelTimer(long handle);

    // Marshal a callback from any thread back to the main thread.
    void PostMainThread(Action<object?> cb, object? state);

    bool IsMainThread { get; }

    // Monotonic frame / tick counter; advances once per loop iteration.
    long CurrentFrame { get; }
}
