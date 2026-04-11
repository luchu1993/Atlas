using System;
using System.Collections.Concurrent;
using System.Threading;

namespace Atlas.Core;

/// <summary>
/// Custom SynchronizationContext ensuring await continuations execute on the main thread.
/// Same principle as Unity's UnitySynchronizationContext.
/// Install via <see cref="SynchronizationContext.SetSynchronizationContext"/> during bootstrap.
/// Call <see cref="ProcessQueue"/> each tick before entity updates.
/// </summary>
internal sealed class AtlasSynchronizationContext : SynchronizationContext
{
    private readonly ConcurrentQueue<WorkItem> _queue = new();
    private readonly int _mainThreadId;

    public AtlasSynchronizationContext()
    {
        _mainThreadId = Environment.CurrentManagedThreadId;
    }

    /// <summary>
    /// Async callback posting (await continuations are routed here).
    /// </summary>
    public override void Post(SendOrPostCallback d, object? state)
    {
        _queue.Enqueue(new WorkItem(d, state, null));
    }

    /// <summary>
    /// Synchronous callback. Executes inline if already on main thread,
    /// otherwise enqueues and blocks until processed.
    /// </summary>
    public override void Send(SendOrPostCallback d, object? state)
    {
        if (Environment.CurrentManagedThreadId == _mainThreadId)
        {
            d(state);
            return;
        }

        using var signal = new ManualResetEventSlim(false);
        _queue.Enqueue(new WorkItem(d, state, signal));
        signal.Wait();
    }

    /// <summary>
    /// Drain all queued continuations on the main thread.
    /// Call once per tick, before EntityManager.OnTickAll.
    /// </summary>
    public int ProcessQueue()
    {
        int processed = 0;
        while (_queue.TryDequeue(out var item))
        {
            try
            {
                item.Callback(item.State);
            }
            catch (Exception ex)
            {
                Log.Error($"Unhandled exception in async continuation: {ex.Message}");
            }
            finally
            {
                item.Signal?.Set();
            }
            processed++;
        }
        return processed;
    }

    private readonly record struct WorkItem(
        SendOrPostCallback Callback, object? State,
        ManualResetEventSlim? Signal);
}
