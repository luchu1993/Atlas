using System;

namespace Atlas.Rpc;

public sealed class RpcDispatchException : Exception
{
    public string EntityOrComponent { get; }
    public string MethodName { get; }
    public uint RpcId { get; }

    public RpcDispatchException(string entityOrComponent, string methodName, uint rpcId,
                                Exception inner)
        : base($"Failed to dispatch {entityOrComponent}.{methodName} (rpcId=0x{rpcId:X8}): {inner.Message}",
               inner)
    {
        EntityOrComponent = entityOrComponent;
        MethodName = methodName;
        RpcId = rpcId;
    }
}
