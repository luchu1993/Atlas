using System;
using Atlas.Serialization;

namespace Atlas.Coro.Rpc;

// Reply wire: [request_id: u32][error_code: i32]([error_msg] | [body]).
// All failure paths go through errorMapper, so awaits never throw.
public sealed class AtlasRpcSource<T> : IAtlasTaskSource<T>, IAtlasRpcCallback
{
    public delegate T SpanDeserializer(ref SpanReader reader);
    public delegate T ErrorMapper(int code, string message);

    private AtlasTaskCompletionSourceCore<T> _core;
    private SpanDeserializer? _deserializer;
    private ErrorMapper? _errorMapper;
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
        SpanDeserializer deserializer, ErrorMapper errorMapper,
        int timeoutMs = AtlasRpc.DefaultTimeoutMs, AtlasCancellationToken ct = default)
    {
        if (registry is null) throw new ArgumentNullException(nameof(registry));
        if (deserializer is null) throw new ArgumentNullException(nameof(deserializer));
        if (errorMapper is null) throw new ArgumentNullException(nameof(errorMapper));

        _registry = registry;
        _deserializer = deserializer;
        _errorMapper = errorMapper;

        if (ct.IsCancellationRequested)
        {
            _core.TrySetResult(errorMapper(RpcErrorCodes.Cancelled, RpcFrameworkMessages.Cancelled));
            return;
        }

        _pendingHandle = registry.RegisterPending(replyId, requestId, timeoutMs, this);

        if (ct.CanBeCanceled)
            _cancelReg = ct.Register(CancelTokenCallback, this);
    }

    private void OnCancellationRequested()
    {
        var reg = _registry;
        var handle = _pendingHandle;
        if (reg is not null && handle != 0) reg.CancelPending(handle);
    }

    public void OnReply(ReadOnlySpan<byte> payload)
    {
        var reader = new SpanReader(payload);
        try
        {
            _ = reader.ReadUInt32();
            int errorCode = reader.ReadInt32();
            if (errorCode != 0)
            {
                string msg;
                try { msg = reader.ReadString(); }
                catch { msg = ""; }
                _core.TrySetResult(_errorMapper!(errorCode, msg));
                return;
            }
        }
        catch (Exception ex)
        {
            _core.TrySetResult(_errorMapper!(RpcErrorCodes.PayloadMalformed, ex.Message));
            return;
        }

        T value;
        try { value = _deserializer!(ref reader); }
        catch (Exception ex)
        {
            _core.TrySetResult(_errorMapper!(RpcErrorCodes.PayloadMalformed, ex.Message));
            return;
        }
        _core.TrySetResult(value);
    }

    public void OnError(RpcCompletionStatus status)
    {
        var (code, msg) = status switch
        {
            RpcCompletionStatus.Timeout      => (RpcErrorCodes.Timeout,      RpcFrameworkMessages.Timeout),
            RpcCompletionStatus.Cancelled    => (RpcErrorCodes.Cancelled,    RpcFrameworkMessages.Cancelled),
            RpcCompletionStatus.ReceiverGone => (RpcErrorCodes.ReceiverGone, RpcFrameworkMessages.ReceiverGone),
            _                                => (RpcErrorCodes.SendFailed,  RpcFrameworkMessages.SendFailed),
        };
        _core.TrySetResult(_errorMapper!(code, msg));
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
        _errorMapper = null;
        _pendingHandle = 0;
        _core.Reset();
        TaskPool<AtlasRpcSource<T>>.Return(this);
    }
}
