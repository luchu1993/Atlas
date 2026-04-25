using System;

namespace Atlas.Client;

// ============================================================================
// ClientHost — host-specific entry points exposed to pure-managed Atlas.Client.
//
// Atlas.Client targets netstandard2.1 and must stay free of any particular
// runtime's native-interop story (hostfxr / Mono / IL2CPP). Everything that
// eventually calls into a native DLL (RPC send, entity-type registry) goes
// through the delegate slots below; the host app wires them up at startup:
//
//   * atlas_client.exe (desktop CoreCLR host): Atlas.Client.Desktop fills
//     the slots with P/Invoke calls to atlas_engine via
//     Atlas.Client.ClientNativeApi.
//
//   * Unity (Mono/IL2CPP host): the Unity package fills the slots with
//     P/Invoke calls to atlas_net_client.dll.
//
// If a slot is left null, the corresponding Atlas.Client call throws
// `InvalidOperationException` with a clear message instead of silently
// dropping the work.
// ============================================================================

public static class ClientHost
{
    public delegate void SendRpcFn(uint entityId, uint rpcId, ReadOnlySpan<byte> payload);
    public delegate void RegisterEntityTypeFn(ReadOnlySpan<byte> data);
    public delegate void RegisterStructFn(ReadOnlySpan<byte> data);
    public delegate void ReportEventSeqGapFn(uint entityId, uint gapDelta);

    public static SendRpcFn? SendBaseRpcHandler;
    public static SendRpcFn? SendCellRpcHandler;
    public static RegisterEntityTypeFn? RegisterEntityTypeHandler;
    public static RegisterStructFn? RegisterStructHandler;
    // Optional — clients that don't route to a BaseApp (editor previews,
    // offline tests) leave this null and the report is silently dropped.
    public static ReportEventSeqGapFn? ReportEventSeqGapHandler;

    internal static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        if (SendBaseRpcHandler is null)
            throw new InvalidOperationException(
                "ClientHost.SendBaseRpcHandler is not set — the host app (Atlas.Client.Desktop "
                + "or the Unity package) must install its P/Invoke handler at startup.");
        SendBaseRpcHandler(entityId, rpcId, payload);
    }

    internal static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        if (SendCellRpcHandler is null)
            throw new InvalidOperationException(
                "ClientHost.SendCellRpcHandler is not set — the host app (Atlas.Client.Desktop "
                + "or the Unity package) must install its P/Invoke handler at startup.");
        SendCellRpcHandler(entityId, rpcId, payload);
    }

    internal static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        if (RegisterEntityTypeHandler is null)
            throw new InvalidOperationException(
                "ClientHost.RegisterEntityTypeHandler is not set — the host app must install "
                + "its entity-type registry bridge at startup.");
        RegisterEntityTypeHandler(data);
    }

    internal static void RegisterStruct(ReadOnlySpan<byte> data)
    {
        if (RegisterStructHandler is null)
            throw new InvalidOperationException(
                "ClientHost.RegisterStructHandler is not set — the host app must install "
                + "its struct registry bridge at startup.");
        RegisterStructHandler(data);
    }

    internal static void ReportEventSeqGap(uint entityId, uint gapDelta)
    {
        // Non-fatal telemetry hop: if the host didn't wire it (Unity in
        // offline mode, test fixtures), drop the report rather than
        // throwing from deep inside a packet decoder.
        ReportEventSeqGapHandler?.Invoke(entityId, gapDelta);
    }
}
