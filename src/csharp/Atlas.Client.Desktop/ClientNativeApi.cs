using System;
using System.Runtime.InteropServices;

namespace Atlas.Client;

// Desktop-only C# → C++ interop against atlas_engine; Unity uses its own
// P/Invoke layer against atlas_net_client.dll instead.
internal static unsafe partial class ClientNativeApi
{
    private const string LibName = "atlas_engine";

    [LibraryImport(LibName, EntryPoint = "AtlasLogMessage")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessageNative(level, ptr, message.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendBaseRpc")]
    private static partial void SendBaseRpcNative(uint entityId, uint rpcId, byte* payload, int len,
                                                  ulong traceId);

    public static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                                   ulong traceId)
    {
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, rpcId, ptr, payload.Length, traceId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendCellRpc")]
    private static partial void SendCellRpcNative(uint entityId, uint rpcId, byte* payload, int len,
                                                  ulong traceId);

    public static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                                   ulong traceId)
    {
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, rpcId, ptr, payload.Length, traceId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasRegisterEntityType")]
    private static partial void RegisterEntityTypeNative(byte* data, int len);

    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            RegisterEntityTypeNative(ptr, data.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasRegisterStruct")]
    private static partial void RegisterStructNative(byte* data, int len);

    public static void RegisterStruct(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            RegisterStructNative(ptr, data.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetEntityDefDigest")]
    private static partial void SetEntityDefDigestNative(byte* data, int len);

    public static void SetEntityDefDigest(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            SetEntityDefDigestNative(ptr, data.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetNativeCallbacks")]
    private static partial void SetNativeCallbacksNative(void* callbacks, int len);

    public static void SetNativeCallbacks(void* callbacks, int len)
    {
        SetNativeCallbacksNative(callbacks, len);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasGetAbiVersion")]
    public static partial uint GetAbiVersion();

    [LibraryImport(LibName, EntryPoint = "AtlasReportClientEventSeqGap")]
    private static partial void ReportClientEventSeqGapNative(uint entityId, uint gapDelta);

    public static void ReportEventSeqGap(uint entityId, uint gapDelta)
    {
        ReportClientEventSeqGapNative(entityId, gapDelta);
    }
}
