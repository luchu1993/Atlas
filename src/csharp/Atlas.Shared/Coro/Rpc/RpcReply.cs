using System;

namespace Atlas.Coro.Rpc;

// Error == 0 = success; > 0 = business; < 0 = framework (RpcErrorCodes).
public readonly struct RpcReply<T>
{
    private readonly T _value;
    public int Error { get; }
    public string? ErrorMessage { get; }

    public bool IsOk             => Error == 0;
    public bool IsBusinessError  => Error > 0;
    public bool IsFrameworkError => Error < 0;

    public T Value => IsOk ? _value
        : throw new InvalidOperationException(
            $"RpcReply.Value on error: code={Error} msg={ErrorMessage}");

    public bool TryGetValue(out T value) { value = _value; return IsOk; }

    public static RpcReply<T> Ok(T v)                   => new(v, 0, null);
    public static RpcReply<T> Fail(int code, string? m) => new(default!, code, m);

    private RpcReply(T v, int e, string? m) { _value = v; Error = e; ErrorMessage = m; }

    // Lets receivers `return RpcReply<T>.Ok(v)` from an AtlasTask-returning method.
    public static implicit operator AtlasTask<RpcReply<T>>(RpcReply<T> reply)
        => AtlasTask<RpcReply<T>>.FromResult(reply);
}
