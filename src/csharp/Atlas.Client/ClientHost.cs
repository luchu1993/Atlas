using System;

namespace Atlas.Client;

// Delegate slots wired by the host app at startup (Atlas.Client.Desktop or the Unity package).
// Unset slots throw InvalidOperationException on call rather than silently dropping work.
public static class ClientHost
{
    public delegate void SendRpcFn(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                                   ulong traceId);
    public delegate void RegisterEntityTypeFn(ReadOnlySpan<byte> data);
    public delegate void RegisterStructFn(ReadOnlySpan<byte> data);
    public delegate void ReportEventSeqGapFn(uint entityId, uint gapDelta);

    public static SendRpcFn? SendBaseRpcHandler;
    public static SendRpcFn? SendCellRpcHandler;
    public static RegisterEntityTypeFn? RegisterEntityTypeHandler;
    public static RegisterStructFn? RegisterStructHandler;
    // Optional: hosts without a BaseApp route (editor previews, tests) leave null; report is dropped.
    public static ReportEventSeqGapFn? ReportEventSeqGapHandler;

    // Build-time SHA-256 of the entity-def surface; LoginRequest carries it so mismatched builds bounce.
    public static byte[]? EntityDefDigest { get; private set; }

    private static T Required<T>(T? handler, string name) where T : Delegate
        => handler ?? throw new InvalidOperationException(
            $"ClientHost.{name} is not set — the host app (Atlas.Client.Desktop or the Unity package) "
            + "must install its handler at startup.");

    internal static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                                     ulong traceId)
        => Required(SendBaseRpcHandler, nameof(SendBaseRpcHandler))(entityId, rpcId, payload, traceId);

    internal static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                                     ulong traceId)
        => Required(SendCellRpcHandler, nameof(SendCellRpcHandler))(entityId, rpcId, payload, traceId);

    internal static void RegisterEntityType(ReadOnlySpan<byte> data)
        => Required(RegisterEntityTypeHandler, nameof(RegisterEntityTypeHandler))(data);

    internal static void RegisterStruct(ReadOnlySpan<byte> data)
        => Required(RegisterStructHandler, nameof(RegisterStructHandler))(data);

    internal static void SetEntityDefDigest(ReadOnlySpan<byte> data)
        => EntityDefDigest = data.ToArray();

    internal static void ReportEventSeqGap(uint entityId, uint gapDelta)
        => ReportEventSeqGapHandler?.Invoke(entityId, gapDelta);
}
