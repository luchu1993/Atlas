using System;
using System.Threading;

namespace Atlas.Coro.Rpc;

// Caller still sends the request payload itself (with requestId prepended
// and rpc_id's kReplyBit set); this just wires up the await side.
public static class AtlasRpc
{
    private static long _nextRequestId;

    public const int DefaultTimeoutMs = 10_000;

    public static uint NextRequestId() =>
        unchecked((uint)Interlocked.Increment(ref _nextRequestId));

    public static AtlasTask<RpcReply<U>> Await<U>(int replyId, uint requestId,
        AtlasRpcSource<RpcReply<U>>.SpanDeserializer deserializer,
        int timeoutMs = DefaultTimeoutMs, AtlasCancellationToken ct = default)
        => Await(AtlasRpcRegistryHost.Current, replyId, requestId, deserializer, timeoutMs, ct);

    public static AtlasTask<RpcReply<U>> Await<U>(IAtlasRpcRegistry registry,
        int replyId, uint requestId,
        AtlasRpcSource<RpcReply<U>>.SpanDeserializer deserializer,
        int timeoutMs = DefaultTimeoutMs, AtlasCancellationToken ct = default)
    {
        if (registry is null) throw new ArgumentNullException(nameof(registry));
        var src = AtlasRpcSource<RpcReply<U>>.Rent();
        src.Start(registry, replyId, requestId,
            deserializer, RpcReplyHelpers.For<U>(),
            timeoutMs, ct);
        return src.Task;
    }

    public static async AtlasTask<T> OrThrow<T>(this AtlasTask<RpcReply<T>> task)
    {
        var reply = await task;
        return reply.IsOk ? reply.Value
            : throw new RpcException(reply.Error, reply.ErrorMessage);
    }
}
