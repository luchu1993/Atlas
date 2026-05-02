using System;
using System.Collections.Generic;

namespace Atlas.Coro.Rpc;

// In-memory IAtlasRpcRegistry. Client uses it directly; server tests too.
// Single-threaded — all calls expected on the IAtlasLoop main thread.
public sealed class ManagedRpcRegistry : IAtlasRpcRegistry, IDisposable
{
    private readonly IAtlasLoop _loop;
    private readonly Dictionary<long, Pending> _byHandle = new();
    private readonly Dictionary<long, long> _byKey = new();   // packed (replyId, requestId) -> handle
    private long _nextHandle;
    private bool _disposed;

    private static readonly Action<object?> TimerFiredCallback =
        static state =>
        {
            var (registry, handle) = ((ManagedRpcRegistry, long))state!;
            registry.OnTimeout(handle);
        };

    public ManagedRpcRegistry(IAtlasLoop loop)
    {
        _loop = loop ?? throw new ArgumentNullException(nameof(loop));
    }

    public int PendingCount => _byHandle.Count;

    public long RegisterPending(int replyId, uint requestId, int timeoutMs,
        IAtlasRpcCallback callback)
    {
        if (callback is null) throw new ArgumentNullException(nameof(callback));
        if (_disposed) throw new ObjectDisposedException(nameof(ManagedRpcRegistry));

        var key = PackKey(replyId, requestId);

        // Overwrite-on-duplicate matches C++ PendingRpcRegistry semantics.
        if (_byKey.TryGetValue(key, out var oldHandle))
        {
            CancelInternal(oldHandle, RpcCompletionStatus.Cancelled);
        }

        var handle = ++_nextHandle;
        long timerHandle = 0;
        if (timeoutMs > 0)
        {
            timerHandle = _loop.RegisterTimer(timeoutMs, TimerFiredCallback, (this, handle));
        }
        _byHandle[handle] = new Pending
        {
            Key = key,
            Callback = callback,
            TimerHandle = timerHandle,
        };
        _byKey[key] = handle;
        return handle;
    }

    public void CancelPending(long handle)
    {
        CancelInternal(handle, RpcCompletionStatus.Cancelled);
    }

    // Returns true if a pending entry claimed the reply.
    public bool TryDispatch(int replyId, uint requestId, ReadOnlySpan<byte> payload)
    {
        var key = PackKey(replyId, requestId);
        if (!_byKey.TryGetValue(key, out var handle)) return false;
        if (!_byHandle.TryGetValue(handle, out var pending)) return false;

        if (pending.TimerHandle != 0) _loop.CancelTimer(pending.TimerHandle);
        _byHandle.Remove(handle);
        _byKey.Remove(key);

        pending.Callback.OnReply(payload);
        return true;
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;

        var snapshot = new List<long>(_byHandle.Keys);
        foreach (var h in snapshot)
            CancelInternal(h, RpcCompletionStatus.Cancelled);
    }

    private void OnTimeout(long handle)
    {
        if (!_byHandle.TryGetValue(handle, out var pending)) return;
        _byHandle.Remove(handle);
        _byKey.Remove(pending.Key);
        pending.Callback.OnError(RpcCompletionStatus.Timeout);
    }

    private void CancelInternal(long handle, RpcCompletionStatus status)
    {
        if (!_byHandle.TryGetValue(handle, out var pending)) return;
        _byHandle.Remove(handle);
        _byKey.Remove(pending.Key);
        if (pending.TimerHandle != 0) _loop.CancelTimer(pending.TimerHandle);
        pending.Callback.OnError(status);
    }

    private static long PackKey(int replyId, uint requestId)
        => ((long)(uint)replyId << 32) | requestId;

    private struct Pending
    {
        public long Key;
        public IAtlasRpcCallback Callback;
        public long TimerHandle;
    }
}
