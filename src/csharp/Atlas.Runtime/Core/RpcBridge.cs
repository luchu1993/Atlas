using System;
using Atlas.Entity;
using Atlas.Serialization;

namespace Atlas.Core;

// Generated dispatchers register here at module-init time so that
// NativeCallbacks.DispatchRpc can route incoming RPCs.
public static class RpcBridge
{
    // replyChannel is a Channel* recast (nullptr = in-process); receivers
    // hand it back via NativeApi.SendEntityRpcReply when a reply-style
    // RPC's user method completes.
    public delegate void RpcDispatchDelegate(ServerEntity entity, int rpcId,
                                             IntPtr replyChannel, ref SpanReader reader);

    // Index: 0 = ClientRpc, 1 = (reserved), 2 = CellRpc, 3 = BaseRpc.
    public static readonly RpcDispatchDelegate?[] Dispatchers = new RpcDispatchDelegate?[4];
}
