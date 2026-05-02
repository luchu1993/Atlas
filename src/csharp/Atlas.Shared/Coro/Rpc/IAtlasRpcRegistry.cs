using System;

namespace Atlas.Coro.Rpc;

public enum RpcCompletionStatus : byte
{
    Success   = 0,
    Timeout   = 1,
    Cancelled = 2,
    SendError = 3,
}

// OnReply must consume payload synchronously — the buffer is not retained.
public interface IAtlasRpcCallback
{
    void OnReply(ReadOnlySpan<byte> payload);
    void OnError(RpcCompletionStatus status);
}

// Implementations: ManagedRpcRegistry (in-memory, client + tests),
// NativeRpcRegistry (server, wraps C++ PendingRpcRegistry).
public interface IAtlasRpcRegistry
{
    // Returns opaque handle for cancellation. timeoutMs <= 0 = no timeout.
    long RegisterPending(int replyId, uint requestId, int timeoutMs, IAtlasRpcCallback callback);

    void CancelPending(long handle);
}
