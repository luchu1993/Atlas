using System;

namespace Atlas.Coro.Rpc;

public sealed class RpcException : Exception
{
    public int ErrorCode { get; }

    public RpcException(int code, string? message) : base(message)
    {
        ErrorCode = code;
    }
}
