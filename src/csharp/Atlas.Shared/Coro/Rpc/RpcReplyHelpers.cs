namespace Atlas.Coro.Rpc;

// Cache<T> JITs once per reply type T, so each For<T>() returns the same
// closureless delegate forever — zero alloc per RPC.
public static class RpcReplyHelpers
{
    public static AtlasRpcSource<RpcReply<T>>.ErrorMapper For<T>() => Cache<T>.ErrorMapper;

    private static class Cache<T>
    {
        public static readonly AtlasRpcSource<RpcReply<T>>.ErrorMapper ErrorMapper =
            static (code, msg) => RpcReply<T>.Fail(code, msg);
    }
}
