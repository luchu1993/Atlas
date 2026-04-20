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

    [LibraryImport(LibName, EntryPoint = "AtlasLogMessage")]
    private static partial void LogMessageNative(int level, byte* msg, int len);

    /// <summary>
    /// Log a UTF-8 message at the given level (0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Critical).
    /// Thread-safe — no ThreadGuard required.
    /// </summary>
    public static void LogMessage(int level, ReadOnlySpan<byte> message)
    {
        fixed (byte* ptr = message)
            LogMessageNative(level, ptr, message.Length);
    }

    // =========================================================================
    // Time
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasServerTime")]
    public static partial double ServerTime();

    [LibraryImport(LibName, EntryPoint = "AtlasDeltaTime")]
    public static partial float DeltaTime();

    // =========================================================================
    // Process identity
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasGetProcessPrefix")]
    public static partial byte GetProcessPrefix();

    // =========================================================================
    // RPC dispatch
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasSendClientRpc")]
    private static partial void SendClientRpcNative(
        uint entityId, uint packedRpcId,
        byte* payload, int payloadLen);

    /// <summary>
    /// Send a client RPC. Direction is encoded in the top 2 bits of packedRpcId
    /// using the format: [direction:2 | typeIndex:14 | method:8].
    /// </summary>
    public static void SendClientRpc(uint entityId, uint packedRpcId,
        ReadOnlySpan<byte> payload)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = payload)
            SendClientRpcNative(entityId, packedRpcId, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendCellRpc")]
    private static partial void SendCellRpcNative(
        uint entityId, uint rpcId, byte* payload, int payloadLen);

    public static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendBaseRpc")]
    private static partial void SendBaseRpcNative(
        uint entityId, uint rpcId, byte* payload, int payloadLen);

    public static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, rpcId, ptr, payload.Length);
    }

    // =========================================================================
    // Entity type registry
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasRegisterEntityType")]
    private static partial void RegisterEntityTypeNative(byte* data, int len);

    public static void RegisterEntityType(ReadOnlySpan<byte> data)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = data)
            RegisterEntityTypeNative(ptr, data.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasUnregisterAllEntityTypes")]
    public static partial void UnregisterAllEntityTypes();

    [LibraryImport(LibName, EntryPoint = "AtlasGiveClientTo")]
    public static partial void GiveClientTo(uint srcEntityId, uint destEntityId);

    [LibraryImport(LibName, EntryPoint = "AtlasCreateBaseEntity")]
    private static partial uint CreateBaseEntityNative(ushort typeId, uint spaceId, float aoiRadius);

    /// <summary>
    /// Script-initiated entity creation on the caller's BaseApp. Returns
    /// the newly-allocated entity id, or 0 on failure. The C# instance is
    /// available from EntityManager.Instance.Get(...) after the call — the
    /// native side invokes RestoreEntity synchronously before returning.
    /// For has_cell types the call also fires CreateCellEntity to a CellApp
    /// targeting <paramref name="spaceId"/> (CellApp auto-creates the Space
    /// if missing) and enables a witness with <paramref name="aoiRadius"/>
    /// (0 = no witness). space_id / aoi_radius are ignored for base-only
    /// types.
    /// </summary>
    public static uint CreateBaseEntity(ushort typeId, uint spaceId = 1, float aoiRadius = 0f)
    {
        ThreadGuard.EnsureMainThread();
        return CreateBaseEntityNative(typeId, spaceId, aoiRadius);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetNativeCallbacks")]
    private static partial void SetNativeCallbacksNative(void* nativeCallbacks, int len);

    public static void SetNativeCallbacks(void* nativeCallbacks, int len)
    {
        SetNativeCallbacksNative(nativeCallbacks, len);
    }

    // =========================================================================
    // ABI version (diagnostic)
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasGetAbiVersion")]
    public static partial uint GetAbiVersion();
}
