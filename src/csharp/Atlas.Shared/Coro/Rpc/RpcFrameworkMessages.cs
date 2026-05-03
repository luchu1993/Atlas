namespace Atlas.Coro.Rpc;

// Interned so framework-error replies don't allocate.
public static class RpcFrameworkMessages
{
    public const string Timeout          = "framework: timeout";
    public const string Cancelled        = "framework: cancelled";
    public const string ReceiverGone     = "framework: receiver gone";
    public const string SendFailed       = "framework: send failed";
    public const string MethodNotFound   = "framework: method not found";
    public const string PayloadMalformed = "framework: payload malformed";
    public const string RemoteException  = "framework: remote exception";
}
