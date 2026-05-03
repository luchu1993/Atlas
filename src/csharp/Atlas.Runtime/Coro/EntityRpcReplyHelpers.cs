using System;
using Atlas.Core;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Serialization;

namespace Atlas.Runtime.Coro;

// Generated dispatchers call SendReplyOnComplete after invoking a user
// reply-style RPC method. Synchronous completions short-circuit; async
// ones register a continuation that fires the reply via NativeApi when
// the task settles.
public static class EntityRpcReplyHelpers
{
    public delegate void ReplySerializer<T>(ref SpanWriter writer, T value);

    private const int kRemoteExceptionMessageMax = 512;

    public static void SendReplyOnComplete<T>(AtlasTask<RpcReply<T>> task,
        IntPtr replyChannel, uint requestId, ReplySerializer<T> serializer)
    {
        var awaiter = task.GetAwaiter();
        if (awaiter.IsCompleted)
        {
            Send(awaiter, replyChannel, requestId, serializer);
            return;
        }
        awaiter.UnsafeOnCompleted(() => Send(awaiter, replyChannel, requestId, serializer));
    }

    private static void Send<T>(AtlasTask<RpcReply<T>>.Awaiter awaiter,
        IntPtr replyChannel, uint requestId, ReplySerializer<T> serializer)
    {
        RpcReply<T> reply;
        try { reply = awaiter.GetResult(); }
        catch (Exception ex)
        {
            var msg = ex.Message ?? "";
            if (msg.Length > kRemoteExceptionMessageMax)
                msg = msg.Substring(0, kRemoteExceptionMessageMax);
            NativeApi.SendEntityRpcFailure(replyChannel, requestId,
                RpcErrorCodes.RemoteException, msg);
            return;
        }

        if (replyChannel == IntPtr.Zero) return;

        if (reply.IsOk)
        {
            var body = new SpanWriter(256);
            try
            {
                serializer(ref body, reply.Value);
                NativeApi.SendEntityRpcSuccess(replyChannel, requestId, body.WrittenSpan);
            }
            finally { body.Dispose(); }
        }
        else
        {
            NativeApi.SendEntityRpcFailure(replyChannel, requestId, reply.Error, reply.ErrorMessage);
        }
    }
}
