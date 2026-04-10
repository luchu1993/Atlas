using System;
using System.Runtime.InteropServices;

namespace Atlas.Core;

// ============================================================================
// NativeApi — C# → C++ interop via [LibraryImport]
// ============================================================================
//
// All declarations here bind to functions exported from atlas_engine.dll/.so
// (built as a CMake SHARED library from src/lib/clrscript/clr_native_api.hpp).
//
// Naming convention:
//   - Private `*Native` methods: raw [LibraryImport] with pointer parameters.
//   - Public wrapper methods: safe API converting Span/string → pointer+length.
//
// Thread safety:
//   All functions forward to INativeApiProvider; see that interface for
//   thread-safety guarantees per function.

internal static unsafe partial class NativeApi
{
    private const string LibName = "atlas_engine";

    // =========================================================================
    // Logging
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_log_message")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    /// <summary>
    /// Log a UTF-8 message at the given level (0=Debug 1=Trace 2=Info 3=Warn 4=Error 5=Critical).
    /// </summary>
    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessageNative(level, ptr, message.Length);
    }

    // =========================================================================
    // Time
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_server_time")]
    public static partial double ServerTime();

    [LibraryImport(LibName, EntryPoint = "atlas_delta_time")]
    public static partial float DeltaTime();

    // =========================================================================
    // Process identity
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_get_process_prefix")]
    public static partial byte GetProcessPrefix();

    // =========================================================================
    // RPC dispatch
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_send_client_rpc")]
    private static partial void SendClientRpcNative(
        uint entityId, uint rpcId, byte target,
        byte* payload, int payloadLen);

    public static void SendClientRpc(uint entityId, uint rpcId, byte target,
        ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendClientRpcNative(entityId, rpcId, target, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "atlas_send_cell_rpc")]
    private static partial void SendCellRpcNative(
        uint entityId, uint rpcId, byte* payload, int payloadLen);

    public static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "atlas_send_base_rpc")]
    private static partial void SendBaseRpcNative(
        uint entityId, uint rpcId, byte* payload, int payloadLen);

    public static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, rpcId, ptr, payload.Length);
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

    [LibraryImport(LibName, EntryPoint = "atlas_unregister_all_entity_types")]
    public static partial void UnregisterAllEntityTypes();

    // =========================================================================
    // ABI version (diagnostic)
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "atlas_get_abi_version")]
    public static partial uint GetAbiVersion();
}
