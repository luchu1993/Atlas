using System;
using System.Runtime.InteropServices;
using Atlas.Entity;
using Atlas.Serialization;

namespace Atlas.Core;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal unsafe struct NativeCallbackTable
{
    public nint RestoreEntity;
    public nint GetEntityData;
    public nint EntityDestroyed;
    public nint DispatchRpc;
    // Appended for补强一 baseline snapshots. C++ tolerates older runtimes where
    // this field is absent; new C++ versions tolerate older runtimes too.
    public nint GetOwnerSnapshot;
    // Phase 11 PR-6: Offload serialization. CellApp invokes this from
    // BuildOffloadMessage to capture the full entity state the destination
    // CellApp will hand to RestoreEntity. Distinct from GetEntityData
    // (DB persistence) and SerializeFor*Client (AoI replication); see
    // docs/roadmap/phase11_distributed_space.md §4 for why.
    public nint SerializeEntity;
    // Phase 11 C8 / §10.2 #9 follow-up: raised by ProximityController
    // when a peer crosses the radius boundary. Routed via user_arg so
    // scripts can disambiguate multiple proximity sensors on one
    // entity. is_enter == 1 on inbound crossing, 0 on outbound.
    public nint ProximityEvent;
}

internal static unsafe class NativeCallbacks
{
    private static byte[]? s_lastSerialized;
    private static GCHandle s_lastSerializedHandle;
    // Separate pin for baseline snapshots so they don't trample GetEntityData's buffer
    // (the two callbacks can be live in the same tick).
    private static byte[]? s_lastOwnerSnapshot;
    private static GCHandle s_lastOwnerSnapshotHandle;

    public static void Register()
    {
        NativeCallbackTable table;
        table.RestoreEntity =
            (nint)(delegate* unmanaged<uint, ushort, long, byte*, int, void>)&RestoreEntity;
        table.GetEntityData = (nint)(delegate* unmanaged<uint, byte**, int*, void>)&GetEntityData;
        table.EntityDestroyed = (nint)(delegate* unmanaged<uint, void>)&EntityDestroyed;
        table.DispatchRpc = (nint)(delegate* unmanaged<uint, uint, byte*, int, void>)&DispatchRpc;
        table.GetOwnerSnapshot =
            (nint)(delegate* unmanaged<uint, byte**, int*, void>)&GetOwnerSnapshot;
        table.SerializeEntity =
            (nint)(delegate* unmanaged<uint, byte*, int, int*, int>)&SerializeEntity;
        table.ProximityEvent =
            (nint)(delegate* unmanaged<uint, int, uint, byte, void>)&ProximityEvent;

        NativeApi.SetNativeCallbacks(&table, sizeof(NativeCallbackTable));
    }

    public static void Reset()
    {
        if (s_lastSerializedHandle.IsAllocated)
        {
            s_lastSerializedHandle.Free();
        }
        s_lastSerialized = null;

        if (s_lastOwnerSnapshotHandle.IsAllocated)
        {
            s_lastOwnerSnapshotHandle.Free();
        }
        s_lastOwnerSnapshot = null;
    }

    [UnmanagedCallersOnly]
    public static void RestoreEntity(uint entityId, ushort typeId, long dbid, byte* data, int len)
    {
        try
        {
            ThreadGuard.EnsureMainThread();

            var entity = EntityManager.Instance.Get(entityId);
            var created = false;
            if (entity is null)
            {
                entity = EntityFactory.CreateByTypeId(typeId);
                if (entity is null)
                {
                    Log.Warning($"RestoreEntity: unknown typeId={typeId} entityId={entityId}");
                    return;
                }

                entity.EntityId = entityId;
                EntityManager.Instance.Register(entity);
                created = true;
            }

            if (data != null && len > 0)
            {
                var reader = new SpanReader(new ReadOnlySpan<byte>(data, len));
                entity.Deserialize(ref reader);
            }

            if (created && !entity.IsDestroyed)
            {
                entity.OnInit(isReload: false);
            }

            _ = dbid;
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }

    [UnmanagedCallersOnly]
    public static void GetEntityData(uint entityId, byte** outData, int* outLen)
    {
        try
        {
            if (outData == null || outLen == null)
            {
                return;
            }

            *outData = null;
            *outLen = 0;

            ThreadGuard.EnsureMainThread();

            var entity = EntityManager.Instance.Get(entityId);
            if (entity is null)
            {
                return;
            }

            var writer = new SpanWriter(4096);
            byte[] snapshot;
            try
            {
                entity.Serialize(ref writer);
                snapshot = writer.WrittenSpan.ToArray();
            }
            finally
            {
                writer.Dispose();
            }

            if (s_lastSerializedHandle.IsAllocated)
            {
                s_lastSerializedHandle.Free();
            }

            s_lastSerialized = snapshot;
            s_lastSerializedHandle = GCHandle.Alloc(snapshot, GCHandleType.Pinned);

            *outData = (byte*)s_lastSerializedHandle.AddrOfPinnedObject();
            *outLen = snapshot.Length;
        }
        catch (Exception ex)
        {
            if (outData != null)
            {
                *outData = null;
            }
            if (outLen != null)
            {
                *outLen = -1;
            }
            ErrorBridge.SetError(ex);
        }
    }

    [UnmanagedCallersOnly]
    public static void GetOwnerSnapshot(uint entityId, byte** outData, int* outLen)
    {
        try
        {
            if (outData == null || outLen == null)
            {
                return;
            }

            *outData = null;
            *outLen = 0;

            ThreadGuard.EnsureMainThread();

            var entity = EntityManager.Instance.Get(entityId);
            if (entity is null)
            {
                return;
            }

            var writer = new SpanWriter(4096);
            byte[] snapshot;
            try
            {
                // Default no-op on base class → empty span for entities with no
                // owner-visible props. Caller must tolerate zero-length snapshots.
                entity.SerializeForOwnerClient(ref writer);
                snapshot = writer.WrittenSpan.ToArray();
            }
            finally
            {
                writer.Dispose();
            }

            if (s_lastOwnerSnapshotHandle.IsAllocated)
            {
                s_lastOwnerSnapshotHandle.Free();
            }

            s_lastOwnerSnapshot = snapshot;
            s_lastOwnerSnapshotHandle = GCHandle.Alloc(snapshot, GCHandleType.Pinned);

            *outData = (byte*)s_lastOwnerSnapshotHandle.AddrOfPinnedObject();
            *outLen = snapshot.Length;
        }
        catch (Exception ex)
        {
            if (outData != null) *outData = null;
            if (outLen != null) *outLen = -1;
            ErrorBridge.SetError(ex);
        }
    }

    [UnmanagedCallersOnly]
    public static int SerializeEntity(uint entityId, byte* outBuf, int outBufCap, int* outLen)
    {
        // Zero-copy contract:
        //   - On success return 0 and write the exact byte count to *outLen.
        //   - If outBufCap is insufficient, return the needed size without
        //     writing; C++ reallocs and retries.
        //   - On error return -1 and set *outLen to -1.
        try
        {
            ThreadGuard.EnsureMainThread();

            if (outLen == null)
            {
                return -1;
            }

            var entity = EntityManager.Instance.Get(entityId);
            if (entity is null)
            {
                Log.Warning($"SerializeEntity: unknown entity {entityId}");
                *outLen = -1;
                return -1;
            }

            // Serialize into a temp buffer so we can report the size
            // precisely. Reusing SpanWriter here keeps the code path
            // identical to GetEntityData's Serialize call — i.e. we
            // go through the generator-emitted ServerEntity.Serialize
            // (Phase 10 abstract), NOT a new SerializeFull variant.
            var writer = new SpanWriter(4096);
            byte[] snapshot;
            try
            {
                entity.Serialize(ref writer);
                snapshot = writer.WrittenSpan.ToArray();
            }
            finally
            {
                writer.Dispose();
            }

            if (outBuf == null || outBufCap < snapshot.Length)
            {
                // Short-buffer protocol — caller retries with a bigger buf.
                *outLen = snapshot.Length;
                return snapshot.Length;
            }

            fixed (byte* src = snapshot)
            {
                Buffer.MemoryCopy(src, outBuf, outBufCap, snapshot.Length);
            }
            *outLen = snapshot.Length;
            return 0;
        }
        catch (Exception ex)
        {
            if (outLen != null) *outLen = -1;
            ErrorBridge.SetError(ex);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static void EntityDestroyed(uint entityId)
    {
        try
        {
            ThreadGuard.EnsureMainThread();
            EntityManager.Instance.Destroy(entityId);
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }

    [UnmanagedCallersOnly]
    public static void DispatchRpc(uint entityId, uint rpcId, byte* payload, int len)
    {
        try
        {
            ThreadGuard.EnsureMainThread();

            var entity = EntityManager.Instance.Get(entityId);
            if (entity is null)
            {
                Log.Warning($"DispatchRpc: unknown entity {entityId}");
                return;
            }

            var reader = new SpanReader(new ReadOnlySpan<byte>(payload, len));

            // Determine direction from the packed rpc_id: bits 22-23
            // 0=ClientRpc, 1=reserved (unused), 2=CellRpc, 3=BaseRpc
            int direction = (int)((rpcId >> 22) & 0x3);
            if (direction == 1)
            {
                Log.Warning($"DispatchRpc: direction=1 is reserved (rpcId=0x{rpcId:X6})");
                return;
            }
            int id = (int)rpcId;

            var dispatcher = RpcBridge.Dispatchers[direction];
            dispatcher?.Invoke(entity, id, ref reader);
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }

    // Phase 11 C8: proximity enter/leave bridge from C++ ProximityController.
    // entityId — owner of the sensor; userArg — script's handle to
    // disambiguate multiple sensors per entity; peerEntityId — base_id
    // of the crossing peer (stable across Offload / visible client-side);
    // isEnter — 1 on inbound, 0 on outbound.
    // Script-side hook is on the entity itself; the generated
    // `ServerEntity` base class (or a future generator pass) surfaces
    // `OnProximityEnter(userArg, peerId)` / `OnProximityLeave(userArg, peerId)`
    // that scripts override. Until that hookup lands the bridge routes
    // events through a logging fallback so integration tests can observe
    // the call reached managed code.
    [UnmanagedCallersOnly]
    public static void ProximityEvent(uint entityId, int userArg, uint peerEntityId, byte isEnter)
    {
        try
        {
            ThreadGuard.EnsureMainThread();

            var entity = EntityManager.Instance.Get(entityId);
            if (entity is null)
            {
                Log.Warning($"ProximityEvent: unknown entity {entityId}");
                return;
            }

            // The generator-emitted OnProximityEnter/Leave hook lands in a
            // follow-up; for now log at Debug so runtime tests can observe
            // the event reached managed code and scripts can override via
            // the existing virtuals if they exist.
            if (isEnter != 0)
            {
                Log.Debug(
                    $"ProximityEvent: entity={entityId} userArg={userArg} peer={peerEntityId} ENTER");
            }
            else
            {
                Log.Debug(
                    $"ProximityEvent: entity={entityId} userArg={userArg} peer={peerEntityId} LEAVE");
            }
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }
}
