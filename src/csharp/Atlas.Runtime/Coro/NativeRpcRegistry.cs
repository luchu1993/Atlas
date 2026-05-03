using System;
using Atlas.Core;
using Atlas.Coro.Rpc;

namespace Atlas.Runtime.Coro;

// Delegates to the ScriptApp's C++ PendingRpcRegistry. The managed handle
// is returned to GCHandlePool by NativeCallbacks.OnRpcComplete.
public sealed class NativeRpcRegistry : IAtlasRpcRegistry
{
    public static readonly NativeRpcRegistry Instance = new();

    private NativeRpcRegistry() { }

    public long RegisterPending(int replyId, uint requestId, int timeoutMs,
        IAtlasRpcCallback callback)
    {
        if (callback is null) throw new ArgumentNullException(nameof(callback));

        var managed = GCHandlePool.Rent(callback);
        var handle = NativeApi.CoroRegisterPending(
            (ushort)replyId, requestId, timeoutMs, managed);
        if (handle == 0)
        {
            GCHandlePool.Return(managed);
            callback.OnError(RpcCompletionStatus.SendError);
            return 0;
        }
        return (long)handle;
    }

    public void CancelPending(long handle)
    {
        if (handle == 0) return;
        NativeApi.CoroCancelPending((ulong)handle);
    }
}
