using System;
using System.Runtime.InteropServices;
using Atlas.DataTypes;
using Atlas.Entity;

namespace Atlas.Core;

// Broadcast scope for cell-side server-to-client RPCs. Mirrors atlas::RpcTarget
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

    [LibraryImport(LibName, EntryPoint = "AtlasReportScriptTick")]
    private static partial void ReportScriptTickNative(uint entityId, ulong elapsedUs);

    public static void ReportScriptTick(uint entityId, ulong elapsedUs)
    {
        ThreadGuard.EnsureMainThread();
        ReportScriptTickNative(entityId, elapsedUs);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSendClientRpc")]
    private static partial void SendClientRpcNative(
        uint entityId, uint packedRpcId, byte target,
        byte* payload, int payloadLen, ulong traceId);

    // packedRpcId layout: reply:1 | slot:7 | direction:2 | typeIndex:14 | method:8.
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

    [LibraryImport(LibName, EntryPoint = "AtlasCreateBaseEntityAt")]
    private static partial uint CreateBaseEntityAtNative(ushort typeId, uint spaceId,
        float posX, float posY, float posZ, float dirX, float dirY, float dirZ,
        [MarshalAs(UnmanagedType.U1)] bool onGround);

    // Returns 0 on failure; instance available via EntityManager.Get after return.
    // For has_cell types also fires CreateCellEntity to spaceId's CellApp.
    public static uint CreateBaseEntity(ushort typeId, uint spaceId = 1)
    {
        ThreadGuard.EnsureMainThread();
        return CreateBaseEntityNative(typeId, spaceId);
    }

    public static uint CreateBaseEntity(ushort typeId, uint spaceId,
        Vector3 position, Vector3 direction, bool onGround = false)
    {
        ThreadGuard.EnsureMainThread();
        return CreateBaseEntityAtNative(typeId, spaceId, position.X, position.Y, position.Z,
            direction.X, direction.Y, direction.Z, onGround);
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

    [LibraryImport(LibName, EntryPoint = "AtlasTeleportEntity")]
    private static partial byte TeleportEntityNative(uint entityId, uint targetSpaceId,
        float posX, float posY, float posZ, float dirX, float dirY, float dirZ);

    // Cross-space teleport via offload. true means the resolve request was
    // dispatched; the move completes async once CellAppMgr names the host.
    public static bool TeleportEntity(uint entityId, uint targetSpaceId,
        Vector3 position, Vector3 direction)
    {
        ThreadGuard.EnsureMainThread();
        return TeleportEntityNative(entityId, targetSpaceId, position.X, position.Y, position.Z,
            direction.X, direction.Y, direction.Z) != 0;
    }

    [LibraryImport(LibName, EntryPoint = "AtlasRequestSpawnCellOnly")]
    private static partial byte RequestSpawnCellOnlyNative(ushort typeId, uint spaceId,
        float posX, float posY, float posZ, float dirX, float dirY, float dirZ,
        [MarshalAs(UnmanagedType.U1)] bool onGround);

    // Base to CellApp route; true means message dispatched (id is async).
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
    private static partial void SetSpaceDataNative(
        uint spaceId, ushort keyId, byte* value, int len);

    [LibraryImport(LibName, EntryPoint = "AtlasRemoveSpaceData")]
    private static partial void RemoveSpaceDataNative(uint spaceId, ushort keyId);

    [LibraryImport(LibName, EntryPoint = "AtlasLoadCollisionAsset")]
    private static partial byte LoadCollisionAssetNative(uint spaceId, byte* path, int len);

    [LibraryImport(LibName, EntryPoint = "AtlasLoadNavMesh")]
    private static partial byte LoadNavMeshNative(uint spaceId, byte* collisionPath,
                                                  int collisionLen, byte* paramsPath,
                                                  int paramsLen);

    [LibraryImport(LibName, EntryPoint = "AtlasGetEntitySpaceId")]
    private static partial uint GetEntitySpaceIdNative(uint entityId);

    [LibraryImport(LibName, EntryPoint = "AtlasAddNavMoveController")]
    private static partial int AddNavMoveControllerNative(uint entityId, float destX, float destY,
                                                          float destZ, float speed, int userArg);

    [LibraryImport(LibName, EntryPoint = "AtlasAddTimerController")]
    private static partial int AddTimerControllerNative(uint entityId, float interval,
                                                        [MarshalAs(UnmanagedType.U1)] bool repeat,
                                                        int userArg);

    [LibraryImport(LibName, EntryPoint = "AtlasCancelController")]
    private static partial void CancelControllerNative(uint entityId, int controllerId);

    // Plans a navmesh path on the entity's space and walks it (state migrates
    // on offload). Returns 0 when the entity is unknown or no path exists.
    public static int AddNavMoveController(uint entityId, Vector3 destination, float speed,
                                           int userArg)
    {
        ThreadGuard.EnsureMainThread();
        return AddNavMoveControllerNative(entityId, destination.X, destination.Y, destination.Z,
                                          speed, userArg);
    }

    // Per-entity timer; state migrates with the entity on offload.
    // Returns 0 on failure.
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

    // Owner-authoritative SpaceData write; fans out to peer cellapps and
    // local witnesses.
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

    public static bool LoadCollisionAsset(uint spaceId, string path)
    {
        ThreadGuard.EnsureMainThread();
        if (string.IsNullOrEmpty(path)) return false;
        var bytes = System.Text.Encoding.UTF8.GetBytes(path);
        fixed (byte* p = bytes)
        {
            return LoadCollisionAssetNative(spaceId, p, bytes.Length) != 0;
        }
    }

    public static bool LoadNavMesh(uint spaceId, string collisionPath, string paramsPath)
    {
        ThreadGuard.EnsureMainThread();
        if (string.IsNullOrEmpty(collisionPath) || string.IsNullOrEmpty(paramsPath)) return false;
        var collisionBytes = System.Text.Encoding.UTF8.GetBytes(collisionPath);
        var paramsBytes = System.Text.Encoding.UTF8.GetBytes(paramsPath);
        fixed (byte* cp = collisionBytes)
        fixed (byte* pp = paramsBytes)
        {
            return LoadNavMeshNative(spaceId, cp, collisionBytes.Length, pp,
                                     paramsBytes.Length) != 0;
        }
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

    [LibraryImport(LibName, EntryPoint = "AtlasSetEntityOnGround")]
    private static partial void SetEntityOnGroundNative(
        uint entityId, [MarshalAs(UnmanagedType.U1)] bool onGround);

    public static void SetEntityOnGround(uint entityId, bool onGround)
    {
        ThreadGuard.EnsureMainThread();
        SetEntityOnGroundNative(entityId, onGround);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetMovementIntent")]
    private static partial void SetMovementIntentNative(uint entityId, float dirX, float dirZ,
        float speedMps, ushort buttons);

    public static void SetMovementIntent(uint entityId, float dirX, float dirZ,
        float speedMps, ushort buttons)
    {
        ThreadGuard.EnsureMainThread();
        SetMovementIntentNative(entityId, dirX, dirZ, speedMps, buttons);
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeMovementCommand
    {
        public uint CommandId;
        public ushort SkillId;
        public byte Type;
        public float StartX;
        public float StartY;
        public float StartZ;
        public float TargetX;
        public float TargetY;
        public float TargetZ;
        public ushort DurationMs;
        public ushort ElapsedMs;
        public ushort CurveId;
        public byte InputPolicy;
        public byte CollisionPolicy;
        public byte Priority;
        public uint ServerTick;

        public NativeMovementCommand(MovementCommand command)
        {
            CommandId = command.CommandId;
            SkillId = command.SkillId;
            Type = (byte)command.Type;
            StartX = command.StartPosition.X;
            StartY = command.StartPosition.Y;
            StartZ = command.StartPosition.Z;
            TargetX = command.TargetPosition.X;
            TargetY = command.TargetPosition.Y;
            TargetZ = command.TargetPosition.Z;
            DurationMs = command.DurationMs;
            ElapsedMs = command.ElapsedMs;
            CurveId = command.CurveId;
            InputPolicy = (byte)command.InputPolicy;
            CollisionPolicy = (byte)command.CollisionPolicy;
            Priority = command.Priority;
            ServerTick = command.ServerTick;
        }
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetMovementCommand")]
    private static partial byte SetMovementCommandNative(
        uint entityId, NativeMovementCommand* command);

    public static bool SetMovementCommand(uint entityId, MovementCommand command)
    {
        ThreadGuard.EnsureMainThread();
        var nativeCommand = new NativeMovementCommand(command);
        return SetMovementCommandNative(entityId, &nativeCommand) != 0;
    }

    [LibraryImport(LibName, EntryPoint = "AtlasClearMovementCommand")]
    private static partial byte ClearMovementCommandNative(uint entityId, uint commandId);

    public static bool ClearMovementCommand(uint entityId, uint commandId = 0)
    {
        ThreadGuard.EnsureMainThread();
        return ClearMovementCommandNative(entityId, commandId) != 0;
    }

    [LibraryImport(LibName, EntryPoint = "AtlasSetMovementCurve")]
    private static partial byte SetMovementCurveNative(
        ushort curveId, float* samples, int sampleCount);

    public static bool SetMovementCurve(ushort curveId, ReadOnlySpan<float> samples)
    {
        ThreadGuard.EnsureMainThread();
        fixed (float* samplePtr = samples)
        {
            return SetMovementCurveNative(curveId, samplePtr, samples.Length) != 0;
        }
    }

    [LibraryImport(LibName, EntryPoint = "AtlasGetEntityPosition")]
    private static partial void GetEntityPositionNative(
        uint entityId, float* x, float* y, float* z);

    [LibraryImport(LibName, EntryPoint = "AtlasGetEntityDirection")]
    private static partial void GetEntityDirectionNative(
        uint entityId, float* x, float* y, float* z);

    [LibraryImport(LibName, EntryPoint = "AtlasGetEntityOnGround")]
    [return: MarshalAs(UnmanagedType.U1)]
    private static partial bool GetEntityOnGroundNative(uint entityId);

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

    public static bool GetEntityOnGround(uint entityId)
    {
        ThreadGuard.EnsureMainThread();
        return GetEntityOnGroundNative(entityId);
    }

    [LibraryImport(LibName, EntryPoint = "AtlasTryGetMovementHistorySample")]
    private static partial byte TryGetMovementHistorySampleNative(
        uint entityId, uint serverTick, uint* outServerTick,
        float* outPx, float* outPy, float* outPz,
        float* outVx, float* outVy, float* outVz,
        float* outDx, float* outDy, float* outDz,
        uint* outFlags, uint* outLastSeq);

    public static bool TryGetMovementHistorySample(
        uint entityId, uint serverTick, out uint sampleServerTick,
        out Vector3 position, out Vector3 velocity, out Vector3 direction,
        out uint flags, out uint lastProcessedInputSeq)
    {
        ThreadGuard.EnsureMainThread();
        float px = 0, py = 0, pz = 0;
        float vx = 0, vy = 0, vz = 0;
        float dx = 0, dy = 0, dz = 0;
        uint nativeServerTick = 0;
        uint nativeFlags = 0;
        uint nativeLastSeq = 0;
        byte ok = TryGetMovementHistorySampleNative(
            entityId, serverTick, &nativeServerTick,
            &px, &py, &pz, &vx, &vy, &vz, &dx, &dy, &dz,
            &nativeFlags, &nativeLastSeq);
        sampleServerTick = nativeServerTick;
        position = new Vector3(px, py, pz);
        velocity = new Vector3(vx, vy, vz);
        direction = new Vector3(dx, dy, dz);
        flags = nativeFlags;
        lastProcessedInputSeq = nativeLastSeq;
        return ok != 0;
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

    // Synchronous full-state write to DBApp. Only BaseApp implements this;
    // other process types no-op.
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
