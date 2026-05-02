using System;
using System.Threading;

namespace Atlas.Coro.Rpc;

// Manual "send + await reply" entry point — used until the Mailbox generator
// emits AtlasTask<TReply>-returning proxies.
public static class AtlasRpc
{
    private static long _nextRequestId;

    public const int DefaultTimeoutMs = 10_000;

    public static uint NextRequestId() => unchecked((uint)Interlocked.Increment(ref _nextRequestId));

    // Caller must send the request payload (with requestId prepended) itself.
    public static AtlasTask<T> Await<T>(int replyId, uint requestId,
        AtlasRpcSource<T>.SpanDeserializer deserializer,
        int timeoutMs = DefaultTimeoutMs, AtlasCancellationToken ct = default)
    {
        var src = AtlasRpcSource<T>.Rent();
        src.Start(AtlasRpcRegistryHost.Current, replyId, requestId, deserializer, timeoutMs, ct);
        return src.Task;
    }

    public static AtlasTask<T> Await<T>(IAtlasRpcRegistry registry, int replyId, uint requestId,
        AtlasRpcSource<T>.SpanDeserializer deserializer,
        int timeoutMs = DefaultTimeoutMs, AtlasCancellationToken ct = default)
    {
        if (registry is null) throw new ArgumentNullException(nameof(registry));
        var src = AtlasRpcSource<T>.Rent();
        src.Start(registry, replyId, requestId, deserializer, timeoutMs, ct);
        return src.Task;
    }
}
