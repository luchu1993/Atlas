using System;
using System.Threading;

namespace Atlas.Coro.Rpc;

// Pooled IAtlasTaskSource that awaits one RPC reply. Caller registers the
// source then sends the request; reply / timeout / cancel feeds back via this.
public sealed class AtlasRpcSource<T> : IAtlasTaskSource<T>, IAtlasRpcCallback
{
    public delegate T SpanDeserializer(ReadOnlySpan<byte> payload);

    private AtlasTaskCompletionSourceCore<T> _core;
    private SpanDeserializer? _deserializer;
    private IAtlasRpcRegistry? _registry;
    private long _pendingHandle;
    private CancelRegistration _cancelReg;

    private static readonly Action<object?> CancelTokenCallback =
        static state => ((AtlasRpcSource<T>)state!).OnCancellationRequested();

    public static AtlasRpcSource<T> Rent()
    {
        if (TaskPool<AtlasRpcSource<T>>.TryRent(out var src)) return src!;
        return new AtlasRpcSource<T>();
    }

    public AtlasTask<T> Task => new(this, _core.Version);

    public void Start(IAtlasRpcRegistry registry, int replyId, uint requestId,
        SpanDeserializer deserializer, int timeoutMs, AtlasCancellationToken ct = default)
    {
        if (registry is null) throw new ArgumentNullException(nameof(registry));
        if (deserializer is null) throw new ArgumentNullException(nameof(deserializer));

        _registry = registry;
        _deserializer = deserializer;

        if (ct.IsCancellationRequested)
        {
            _core.TrySetCanceled();
            return;
        }

        _pendingHandle = registry.RegisterPending(replyId, requestId, timeoutMs, this);

        if (ct.CanBeCanceled)
        {
            // If token cancelled between the IsCancellationRequested check
            // and Register, the registry's entry is torn down via CancelPending.
            _cancelReg = ct.Register(CancelTokenCallback, this);
        }
    }

    private void OnCancellationRequested()
    {
        var reg = _registry;
        var handle = _pendingHandle;
        if (reg is not null && handle != 0) reg.CancelPending(handle);
    }

    public void OnReply(ReadOnlySpan<byte> payload)
    {
        T value;
        try { value = _deserializer!(payload); }
        catch (Exception ex) { _core.TrySetException(ex); return; }
        _core.TrySetResult(value);
    }

    public void OnError(RpcCompletionStatus status)
    {
        switch (status)
        {
            case RpcCompletionStatus.Cancelled:
                _core.TrySetCanceled();
                break;
            case RpcCompletionStatus.Timeout:
                _core.TrySetException(new TimeoutException("RPC timed out"));
                break;
            default:
                _core.TrySetException(new InvalidOperationException(
                    $"RPC failed with status {status}"));
                break;
        }
    }

    public short Version => _core.Version;
    public AtlasTaskStatus GetStatus(short token) => _core.GetStatus(token);
    public void OnCompleted(Action<object?> cont, object? state, short token)
        => _core.OnCompleted(cont, state, token);

    public T GetResult(short token)
    {
        try { return _core.GetResult(token); }
        finally { Reset(); }
    }

    private void Reset()
    {
        _cancelReg.Dispose();
        _cancelReg = default;
        _registry = null;
        _deserializer = null;
        _pendingHandle = 0;
        _core.Reset();
        TaskPool<AtlasRpcSource<T>>.Return(this);
    }
}
