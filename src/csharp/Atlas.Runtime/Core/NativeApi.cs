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

    // Thread-safe; level 0=Trace 1=Debug 2=Info 3=Warn 4=Error 5=Critical.
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

    // packedRpcId layout: [direction:2 | typeIndex:14 | method:8].
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

    [LibraryImport(LibName, EntryPoint = "AtlasSetSpaceMasterType")]
    private static partial void SetSpaceMasterTypeNative(uint spaceId, byte* name, int len);

    // Empty / null unregisters. BaseApp-only; other process types treat as no-op.
    public static void SetSpaceMasterType(uint spaceId, string? typeName)
    {
        ThreadGuard.EnsureMainThread();
        if (string.IsNullOrEmpty(typeName))
        {
            SetSpaceMasterTypeNative(spaceId, null, 0);
            return;
        }
        var bytes = System.Text.Encoding.UTF8.GetBytes(typeName);
        fixed (byte* p = bytes) SetSpaceMasterTypeNative(spaceId, p, bytes.Length);
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

    // Must run before any RegisterEntityType whose descriptor references this struct id.
    public static void RegisterStruct(ReadOnlySpan<byte> data)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = data)
            RegisterStructNative(ptr, data.Length);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasRegisterComponent")]
    private static partial void RegisterComponentNative(byte* data, int len);

    // Must run after RegisterStruct and before any RegisterEntityType whose
    // slot table references this component_type_id.
    public static void RegisterComponent(ReadOnlySpan<byte> data)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* ptr = data)
            RegisterComponentNative(ptr, data.Length);
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

    // Returns 0 on failure; instance available via EntityManager.Get after return.
    // For has_cell types also fires CreateCellEntity to spaceId's CellApp.
    public static uint CreateBaseEntity(ushort typeId, uint spaceId = 1)
    {
        ThreadGuard.EnsureMainThread();
        return CreateBaseEntityNative(typeId, spaceId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasCreateLocalCellEntity")]
    private static partial uint CreateLocalCellEntityNative(ushort typeId, uint spaceId,
        float posX, float posY, float posZ, float dirX, float dirY, float dirZ,
        [MarshalAs(UnmanagedType.U1)] bool onGround);

    // Cell-only entity (no Base counterpart, no DB row). 0 on failure.
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

    // Base → CellApp route; true means message dispatched (id is async).
    public static bool RequestSpawnCellOnly(ushort typeId, uint spaceId,
        Vector3 position, Vector3 direction, bool onGround)
    {
        ThreadGuard.EnsureMainThread();
        return RequestSpawnCellOnlyNative(typeId, spaceId, position.X, position.Y, position.Z,
            direction.X, direction.Y, direction.Z, onGround) != 0;
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetAoIRadius")]
    private static partial void SetAoIRadiusNative(uint entityId, float radius, float hysteresis);

    // Cell-side clamps radius to [0.1, max]; hysteresis widens leave boundary.
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

    // CellApp updates CellEntity::position_ + range_node_; other process
    // types log + no-op so shared scripts on the wrong host don't crash.
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

    // C++ runtime owns the seq counters (CellEntity::replication_state_) so
    // they survive Offload. Script only signals which streams advanced.
    [LibraryImport(LibName, EntryPoint = "AtlasPublishReplicationFrame")]
    private static partial void PublishReplicationFrameNative(
        uint entityId, byte hasEvent, byte hasVolatile,
        byte* ownerSnap, int ownerSnapLen,
        byte* otherSnap, int otherSnapLen,
        byte* ownerDelta, int ownerDeltaLen,
        byte* otherDelta, int otherDeltaLen);

    public static void PublishReplicationFrame(uint entityId, bool hasEvent, bool hasVolatile,
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
                entityId, (byte)(hasEvent ? 1 : 0), (byte)(hasVolatile ? 1 : 0),
                ownerSnapPtr, ownerSnap.Length,
                otherSnapPtr, otherSnap.Length,
                ownerDeltaPtr, ownerDelta.Length,
                otherDeltaPtr, otherDelta.Length);
        }
    }

    // Synchronous write of the entity's full state to DBApp. Only meaningful on
    // BaseApp (no-op on other process types). Use for properties that must
    // persist outside the normal logoff-snapshot path (e.g. detached Account).
    [LibraryImport(LibName, EntryPoint = "AtlasWriteToDb")]
    private static partial void WriteToDbNative(uint entityId, byte* entityData, int len);

    public static void WriteEntityToDb(uint entityId, ReadOnlySpan<byte> entityData)
    {
        ThreadGuard.EnsureMainThread();
        fixed (byte* p = entityData)
            WriteToDbNative(entityId, p, entityData.Length);
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
