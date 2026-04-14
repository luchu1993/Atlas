using System;
using System.Runtime.InteropServices;

namespace Atlas.Client;

/// <summary>
/// Client-side C# → C++ interop. Binds to functions exported from the
/// client executable's shared library (atlas_client or atlas_engine).
/// </summary>
internal static unsafe partial class ClientNativeApi
{
    private const string LibName = "atlas_engine";

    // =========================================================================
    // Logging
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_log_message")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessageNative(level, ptr, message.Length);
    }

    // =========================================================================
    // RPC dispatch — client only sends base and cell RPCs
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_send_base_rpc")]
    private static partial void SendBaseRpcNative(uint entityId, uint rpcId, byte* payload, int len);

    public static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "atlas_send_cell_rpc")]
    private static partial void SendCellRpcNative(uint entityId, uint rpcId, byte* payload, int len);

    public static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    // =========================================================================
    // Entity type registry
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_register_entity_type")]
    private static partial void RegisterEntityTypeNative(byte* data, int len);

    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        fixed (byte* ptr = data)
            RegisterEntityTypeNative(ptr, data.Length);
    }

    // =========================================================================
    // Callback registration
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_set_native_callbacks")]
    private static partial void SetNativeCallbacksNative(void* callbacks, int len);

    public static void SetNativeCallbacks(void* callbacks, int len)
    {
        SetNativeCallbacksNative(callbacks, len);
    }

    // =========================================================================
    // ABI version
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_get_abi_version")]
    public static partial uint GetAbiVersion();
}
