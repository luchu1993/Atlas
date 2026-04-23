using System;
using System.Runtime.InteropServices;

namespace Atlas.Client;

/// <summary>
/// Client-side C# → C++ interop. Binds to functions exported from the
/// client executable's shared library (atlas_client or atlas_engine).
/// </summary>
internal static unsafe partial class ClientNativeApi
{
    // atlas_engine exports C API from src/lib/clrscript/ and is specific to
    // the desktop CoreCLR-hosted client (atlas_client.exe). The Unity build
    // path never compiles this assembly — Unity's Mono/IL2CPP host ships
    // its own P/Invoke layer against atlas_net_client.dll with a different
    // C API shape (see docs/UNITY_NATIVE_DLL_DESIGN.md §6.1), so there is
    // no "shared source" case to guard for.
    private const string LibName = "atlas_engine";

    // =========================================================================
    // Logging
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasLogMessage")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessageNative(level, ptr, message.Length);
    }

    // =========================================================================
    // RPC dispatch — client only sends base and cell RPCs
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasSendBaseRpc")]
    private static partial void SendBaseRpcNative(uint entityId, uint rpcId, byte* payload, int len);

    public static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendCellRpc")]
    private static partial void SendCellRpcNative(uint entityId, uint rpcId, byte* payload, int len);

    public static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    // =========================================================================
    // Entity type registry
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasRegisterEntityType")]
    private static partial void RegisterEntityTypeNative(byte* data, int len);

    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            RegisterEntityTypeNative(ptr, data.Length);
    }

    // =========================================================================
    // Callback registration
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasSetNativeCallbacks")]
    private static partial void SetNativeCallbacksNative(void* callbacks, int len);

    public static void SetNativeCallbacks(void* callbacks, int len)
    {
        SetNativeCallbacksNative(callbacks, len);
    }

    // =========================================================================
    // ABI version
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasGetAbiVersion")]
    public static partial uint GetAbiVersion();

    // =========================================================================
    // Telemetry
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasReportClientEventSeqGap")]
    private static partial void ReportClientEventSeqGapNative(uint entityId, uint gapDelta);

    public static void ReportEventSeqGap(uint entityId, uint gapDelta)
    {
        ReportClientEventSeqGapNative(entityId, gapDelta);
    }
}
