using System;
using System.Runtime.InteropServices;
using Atlas.Core;
using Atlas.Coro.Rpc;

namespace Atlas.Runtime.Coro;

// Server impl: delegates to the C++ PendingRpcRegistry owned by the ScriptApp.
// GCHandle is freed by NativeCallbacks.OnRpcComplete on every completion path.
public sealed class NativeRpcRegistry : IAtlasRpcRegistry
{
    public static readonly NativeRpcRegistry Instance = new();

    private NativeRpcRegistry() { }

    public long RegisterPending(int replyId, uint requestId, int timeoutMs,
        IAtlasRpcCallback callback)
    {
        if (callback is null) throw new ArgumentNullException(nameof(callback));

        var gch = GCHandle.Alloc(callback, GCHandleType.Normal);
        var handle = NativeApi.CoroRegisterPending(
            (ushort)replyId, requestId, timeoutMs, GCHandle.ToIntPtr(gch));
        if (handle == 0)
        {
            // Native rejected — surface as SendError so caller's await throws.
            gch.Free();
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
