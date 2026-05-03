namespace Atlas.Coro.Rpc;

// Negative = framework, > 0 = business, 0 = success.
public static class RpcErrorCodes
{
    public const int Timeout          = -1;
    // Covers offload, destroy, hot-reload, and explicit ct cancellation.
    public const int Cancelled        = -2;
    public const int RemoteException  = -3;
    public const int ReceiverGone     = -4;
    public const int SendFailed       = -5;
    public const int MethodNotFound   = -6;
    public const int PayloadMalformed = -7;
}
