using Atlas.Entity;
using Atlas.Serialization;

namespace Atlas.Core;

/// <summary>
/// Public bridge for generated RPC dispatchers to register themselves.
/// Generated code calls <see cref="Dispatchers"/> during module initialisation
/// so that <see cref="NativeCallbacks.DispatchRpc"/> can route incoming RPCs.
/// </summary>
public static class RpcBridge
{
    public delegate void RpcDispatchDelegate(ServerEntity entity, int rpcId, ref SpanReader reader);

    /// <summary>
    /// Per-direction dispatch delegates.
    /// Index: 0 = ClientRpc, 1 = (reserved), 2 = CellRpc, 3 = BaseRpc.
    /// Note: slot 1 was previously ServerRpc, now removed.
    /// Exposed cell/base methods are dispatched via CellRpc (2) / BaseRpc (3);
    /// the exposed attribute is handled by C++ EntityDefRegistry, not C# direction.
    /// </summary>
    public static readonly RpcDispatchDelegate?[] Dispatchers = new RpcDispatchDelegate?[4];
}
