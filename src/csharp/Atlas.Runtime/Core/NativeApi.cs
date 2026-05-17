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


internal static unsafe partial class NativeApi
{
    private const string LibName = "atlas_engine";

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

    [LibraryImport(LibName, EntryPoint = "AtlasServerTime")]
    public static partial double ServerTime();

    [LibraryImport(LibName, EntryPoint = "AtlasDeltaTime")]
    public static partial float DeltaTime();

    [LibraryImport(LibName, EntryPoint = "AtlasGetProcessPrefix")]
    public static partial byte GetProcessPrefix();

    [LibraryImport(LibName, EntryPoint = "AtlasSendClientRpc")]
    private static partial void SendClientRpcNative(
        uint entityId, uint packedRpcId, byte target,
        byte* payload, int payloadLen, ulong traceId);

    /// <summary>
    /// Send a client RPC. Direction is encoded in the top 2 bits of packedRpcId
    /// using the format: [direction:2 | typeIndex:14 | method:8].
    /// </summary>
    public static void SendClientRpc(uint entityId, uint packedRpcId, RpcTarget target,
        ReadOnlySpan<byte> payload, ulong traceId)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = payload)
            SendClientRpcNative(entityId, packedRpcId, (byte)target, ptr, payload.Length, traceId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendCellRpc")]
    private static partial void SendCellRpcNative(
        uint entityId, uint rpcId, byte* payload, int payloadLen, ulong traceId);

    public static void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                                   ulong traceId)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = payload)
            SendCellRpcNative(entityId, rpcId, ptr, payload.Length, traceId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendBaseRpc")]
    private static partial void SendBaseRpcNative(
        uint entityId, uint rpcId, byte* payload, int payloadLen, ulong traceId);

    public static void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                                   ulong traceId)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = payload)
            SendBaseRpcNative(entityId, rpcId, ptr, payload.Length, traceId);
    }

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

    [LibraryImport(LibName, EntryPoint = "AtlasSetEntityDefDigest")]
    private static partial void SetEntityDefDigestNative(byte* data, int len);

    public static void SetEntityDefDigest(ReadOnlySpan<byte> data)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = data)
            SetEntityDefDigestNative(ptr, data.Length);
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

    [LibraryImport(LibName, EntryPoint = "AtlasCreateLocalCellEntity")]
    private static partial uint CreateLocalCellEntityNative(ushort typeId, uint spaceId,
        float posX, float posY, float posZ, float dirX, float dirY, float dirZ,
        [MarshalAs(UnmanagedType.U1)] bool onGround);

    // Cell-script entry: creates a cell-only entity here on this CellApp.
    // No Base counterpart, no DB persistence. Returns 0 on failure.
    public static uint CreateLocalCellEntity(ushort typeId, uint spaceId,
        Vector3 position, Vector3 direction, bool onGround)
    {
        ThreadGuard.EnsureMainThread();
        return CreateLocalCellEntityNative(typeId, spaceId, position.X, position.Y, position.Z,
            direction.X, direction.Y, direction.Z, onGround);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasDestroyCellEntity")]
    private static partial void DestroyCellEntityNative(uint entityId);

    // Cell-script entry: destroys a cell-only entity created via
    // CreateLocalCellEntity. Refuses base-owned entities on the native side.
    public static void DestroyCellEntity(uint entityId)
    {
        ThreadGuard.EnsureMainThread();
        DestroyCellEntityNative(entityId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasRequestSpawnCellOnly")]
    private static partial byte RequestSpawnCellOnlyNative(ushort typeId, uint spaceId,
        float posX, float posY, float posZ, float dirX, float dirY, float dirZ,
        [MarshalAs(UnmanagedType.U1)] bool onGround);

    // Base-script entry: asks the CellApp owning spaceId to spawn a
    // cell-only entity. Returns true if the routing message went out (cell
    // assigns the id asynchronously; not returned synchronously).
    public static bool RequestSpawnCellOnly(ushort typeId, uint spaceId,
        Vector3 position, Vector3 direction, bool onGround)
    {
        ThreadGuard.EnsureMainThread();
        return RequestSpawnCellOnlyNative(typeId, spaceId, position.X, position.Y, position.Z,
            direction.X, direction.Y, direction.Z, onGround) != 0;
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

    [LibraryImport(LibName, EntryPoint = "AtlasSetSpaceData")]
    private static partial void SetSpaceDataNative(uint spaceId, ushort keyId, byte* value, int len);

    [LibraryImport(LibName, EntryPoint = "AtlasRemoveSpaceData")]
    private static partial void RemoveSpaceDataNative(uint spaceId, ushort keyId);

    [LibraryImport(LibName, EntryPoint = "AtlasGetEntitySpaceId")]
    private static partial uint GetEntitySpaceIdNative(uint entityId);

    [LibraryImport(LibName, EntryPoint = "AtlasAddTimerController")]
    private static partial int AddTimerControllerNative(uint entityId, float interval,
                                                        [MarshalAs(UnmanagedType.U1)] bool repeat,
                                                        int userArg);

    [LibraryImport(LibName, EntryPoint = "AtlasCancelController")]
    private static partial void CancelControllerNative(uint entityId, int controllerId);

    /// <summary>Per-entity timer; state migrates with the entity on offload. Returns 0 on failure.</summary>
    public static int AddTimerController(uint entityId, float intervalSeconds, bool repeat,
                                         int userArg)
    {
        ThreadGuard.EnsureMainThread();
        return AddTimerControllerNative(entityId, intervalSeconds, repeat, userArg);
    }

    public static void CancelController(uint entityId, int controllerId)
    {
        ThreadGuard.EnsureMainThread();
        CancelControllerNative(entityId, controllerId);
    }

    /// <summary>Returns the entity's owning space id, or 0 if unknown.</summary>
    public static uint GetEntitySpaceId(uint entityId)
    {
        ThreadGuard.EnsureMainThread();
        return GetEntitySpaceIdNative(entityId);
    }

    /// <summary>Owner-authoritative SpaceData write — fans out to peer cellapps and local witnesses.</summary>
    public static void SetSpaceData(uint spaceId, ushort keyId, ReadOnlySpan<byte> value)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* p = value)
            SetSpaceDataNative(spaceId, keyId, p, value.Length);
    }

    public static void RemoveSpaceData(uint spaceId, ushort keyId)
    {
        ThreadGuard.EnsureMainThread();
        RemoveSpaceDataNative(spaceId, keyId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetNativeCallbacks")]
    private static partial void SetNativeCallbacksNative(void* nativeCallbacks, int len);

    public static void SetNativeCallbacks(void* nativeCallbacks, int len)
    {
        SetNativeCallbacksNative(nativeCallbacks, len);
    }

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

    [LibraryImport(LibName, EntryPoint = "AtlasSetEntityDirection")]
    private static partial void SetEntityDirectionNative(uint entityId, float x, float y, float z);

    public static void SetEntityDirection(uint entityId, Vector3 direction)
    {
        ThreadGuard.EnsureMainThread();
        SetEntityDirectionNative(entityId, direction.X, direction.Y, direction.Z);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasGetEntityPosition")]
    private static partial void GetEntityPositionNative(uint entityId, float* x, float* y, float* z);

    [LibraryImport(LibName, EntryPoint = "AtlasGetEntityDirection")]
    private static partial void GetEntityDirectionNative(uint entityId, float* x, float* y, float* z);

    public static Vector3 GetEntityPosition(uint entityId)
    {
        ThreadGuard.EnsureMainThread();
        float x = 0, y = 0, z = 0;
        GetEntityPositionNative(entityId, &x, &y, &z);
        return new Vector3(x, y, z);
    }

    public static Vector3 GetEntityDirection(uint entityId)
    {
        ThreadGuard.EnsureMainThread();
        float x = 0, y = 0, z = 0;
        GetEntityDirectionNative(entityId, &x, &y, &z);
        return new Vector3(x, y, z);
    }

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

    [LibraryImport(LibName, EntryPoint = "AtlasGetAbiVersion")]
    public static partial uint GetAbiVersion();

    // Returns 0 when the process has no PendingRpcRegistry or callback fn.
    [LibraryImport(LibName, EntryPoint = "AtlasCoroRegisterPending")]
    public static partial ulong CoroRegisterPending(ushort replyId, uint requestId,
        int timeoutMs, IntPtr managedHandle);

    [LibraryImport(LibName, EntryPoint = "AtlasCoroCancelPending")]
    public static partial void CoroCancelPending(ulong handle);

    [LibraryImport(LibName, EntryPoint = "AtlasSendEntityRpcSuccess")]
    public static unsafe partial void SendEntityRpcSuccessRaw(IntPtr replyChannel,
        uint requestId, byte* body, int len);

    public static unsafe void SendEntityRpcSuccess(IntPtr replyChannel, uint requestId,
        ReadOnlySpan<byte> body)
    {
        fixed (byte* p = body)
        {
            SendEntityRpcSuccessRaw(replyChannel, requestId, p, body.Length);
        }
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendEntityRpcFailure")]
    public static unsafe partial void SendEntityRpcFailureRaw(IntPtr replyChannel,
        uint requestId, int errorCode, byte* msg, int msgLen);

    public static unsafe void SendEntityRpcFailure(IntPtr replyChannel, uint requestId,
        int errorCode, string? msg)
    {
        var buf = msg is null
            ? ReadOnlySpan<byte>.Empty
            : System.Text.Encoding.UTF8.GetBytes(msg).AsSpan();
        fixed (byte* p = buf)
        {
            SendEntityRpcFailureRaw(replyChannel, requestId, errorCode, p, buf.Length);
        }
    }
}
