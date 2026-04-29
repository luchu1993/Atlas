using System;
using System.Runtime.InteropServices;
using Atlas.DataTypes;

namespace Atlas.Core;

// Broadcast scope for cell-side server→client RPCs. Mirrors atlas::RpcTarget
// on the C++ side; BaseApp only supports Owner (no AoI graph).
public enum RpcTarget : byte
{
    Owner = 0,
    Others = 1,
    All = 2,
}

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
        uint entityId, uint packedRpcId, byte target,
        byte* payload, int payloadLen);

    /// <summary>
    /// Send a client RPC. Direction is encoded in the top 2 bits of packedRpcId
    /// using the format: [direction:2 | typeIndex:14 | method:8].
    /// </summary>
    public static void SendClientRpc(uint entityId, uint packedRpcId, RpcTarget target,
        ReadOnlySpan<byte> payload)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = payload)
            SendClientRpcNative(entityId, packedRpcId, (byte)target, ptr, payload.Length);
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

    [LibraryImport(LibName, EntryPoint = "AtlasRegisterStruct")]
    private static partial void RegisterStructNative(byte* data, int len);

    // Must be invoked before any RegisterEntityType whose descriptor
    // references this struct by id — RegisterType's decoder resolves
    // struct_id references against the registry's current state.
    public static void RegisterStruct(ReadOnlySpan<byte> data)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = data)
            RegisterStructNative(ptr, data.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasUnregisterAllEntityTypes")]
    public static partial void UnregisterAllEntityTypes();

    [LibraryImport(LibName, EntryPoint = "AtlasGiveClientTo")]
    public static partial void GiveClientTo(uint srcEntityId, uint destEntityId);

    [LibraryImport(LibName, EntryPoint = "AtlasCreateBaseEntity")]
    private static partial uint CreateBaseEntityNative(ushort typeId, uint spaceId);

    /// <summary>
    /// Script-initiated entity creation on the caller's BaseApp. Returns
    /// the newly-allocated entity id, or 0 on failure. The C# instance is
    /// available from EntityManager.Instance.Get(...) after the call — the
    /// native side invokes RestoreEntity synchronously before returning.
    /// For has_cell types the call also fires CreateCellEntity to a CellApp
    /// targeting <paramref name="spaceId"/> (CellApp auto-creates the Space
    /// if missing). space_id is ignored for base-only types.
    /// <para/>
    /// Witness attachment happens via the client-bind path
    /// (<see cref="ServerEntity.GiveClientTo"/> → BaseApp BindClient → cell
    /// EnableWitness). Scripts wanting a non-default AoI radius call
    /// <see cref="ServerEntity.SetAoIRadius"/> after GiveClientTo.
    /// </summary>
    public static uint CreateBaseEntity(ushort typeId, uint spaceId = 1)
    {
        ThreadGuard.EnsureMainThread();
        return CreateBaseEntityNative(typeId, spaceId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetAoIRadius")]
    private static partial void SetAoIRadiusNative(uint entityId, float radius, float hysteresis);

    /// <summary>
    /// Forward a SetAoIRadius to the cell hosting this entity's
    /// counterpart. Radius is clamped on the cell side to [0.1, max];
    /// hysteresis widens the leave boundary so peers inside
    /// (radius, radius+hysteresis) stay in AoI.
    /// </summary>
    public static void SetAoIRadius(uint entityId, float radius, float hysteresis)
    {
        ThreadGuard.EnsureMainThread();
        SetAoIRadiusNative(entityId, radius, hysteresis);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetNativeCallbacks")]
    private static partial void SetNativeCallbacksNative(void* nativeCallbacks, int len);

    public static void SetNativeCallbacks(void* nativeCallbacks, int len)
    {
        SetNativeCallbacksNative(nativeCallbacks, len);
    }

    // =========================================================================
    // CellApp spatial
    // =========================================================================
    //
    // AtlasSetEntityPosition forwards to the active INativeApiProvider. On
    // CellApp that updates the CellEntity's C++ position_ + range_node_ so
    // AoI triggers see the move. On any other process type the provider
    // logs a warning and no-ops — harmless if a shared script accidentally
    // runs there. See src/lib/clrscript/clr_native_api_defs.h.

    [LibraryImport(LibName, EntryPoint = "AtlasSetEntityPosition")]
    private static partial void SetEntityPositionNative(uint entityId, float x, float y, float z);

    public static void SetEntityPosition(uint entityId, Vector3 position)
    {
        ThreadGuard.EnsureMainThread();
        SetEntityPositionNative(entityId, position.X, position.Y, position.Z);
    }

    // =========================================================================
    // Replication frame pump (CellApp)
    // =========================================================================
    //
    // Hand one tick of replication output for a single entity to the cell
    // layer. BuildAndConsumeReplicationFrame on the C# side produces the four
    // audience-filtered buffers (+ event_seq / volatile_seq); this routes
    // them to CellEntity::PublishReplicationFrame on the native side, which
    // updates ReplicationState.history for downstream Witness consumption.
    //
    // The four byte* parameters accept nullptr with the corresponding length
    // == 0 — callers passing empty SpanWriter output skip the `fixed` block.
    // On non-CellApp processes (BaseApp, Client) the provider logs a warning
    // and no-ops, so C# can blind-forward any entity whose frame advanced.

    [LibraryImport(LibName, EntryPoint = "AtlasPublishReplicationFrame")]
    private static partial void PublishReplicationFrameNative(
        uint entityId, ulong eventSeq, ulong volatileSeq,
        byte* ownerSnap, int ownerSnapLen,
        byte* otherSnap, int otherSnapLen,
        byte* ownerDelta, int ownerDeltaLen,
        byte* otherDelta, int otherDeltaLen);

    public static void PublishReplicationFrame(uint entityId, ulong eventSeq, ulong volatileSeq,
        ReadOnlySpan<byte> ownerSnap, ReadOnlySpan<byte> otherSnap,
        ReadOnlySpan<byte> ownerDelta, ReadOnlySpan<byte> otherDelta)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ownerSnapPtr = ownerSnap)
        fixed (byte* otherSnapPtr = otherSnap)
        fixed (byte* ownerDeltaPtr = ownerDelta)
        fixed (byte* otherDeltaPtr = otherDelta)
        {
            PublishReplicationFrameNative(
                entityId, eventSeq, volatileSeq,
                ownerSnapPtr, ownerSnap.Length,
                otherSnapPtr, otherSnap.Length,
                ownerDeltaPtr, ownerDelta.Length,
                otherDeltaPtr, otherDelta.Length);
        }
    }

    // =========================================================================
    // ABI version (diagnostic)
    // =========================================================================

    [LibraryImport(LibName, EntryPoint = "AtlasGetAbiVersion")]
    public static partial uint GetAbiVersion();
}
