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
    // Appended for baseline-snapshot support. C++ tolerates older runtimes
    // where this field is absent; new C++ tolerates older runtimes too.
    public nint GetOwnerSnapshot;
    // Offload serialization. CellApp invokes this from BuildOffloadMessage
    // to capture the full entity state the destination CellApp will hand to
    // RestoreEntity. Distinct from GetEntityData (DB persistence) and
    // SerializeFor*Client (AoI replication).
    public nint SerializeEntity;
    // Raised by ProximityController when a peer crosses the radius
    // boundary. Routed via user_arg so scripts can disambiguate multiple
    // proximity sensors on one entity. is_enter == 1 inbound, 0 outbound.
    public nint ProximityEvent;
    // GCHandle of the IAtlasRpcCallback is returned to GCHandlePool here.
    public nint OnRpcComplete;
    // Triggers the entity's LifecycleCancellation source.
    public nint EntityLifecycleCancel;
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
        table.DispatchRpc =
            (nint)(delegate* unmanaged<uint, uint, IntPtr, byte*, int, void>)&DispatchRpc;
        table.GetOwnerSnapshot =
            (nint)(delegate* unmanaged<uint, byte**, int*, void>)&GetOwnerSnapshot;
        table.SerializeEntity =
            (nint)(delegate* unmanaged<uint, byte*, int, int*, int>)&SerializeEntity;
        table.ProximityEvent =
            (nint)(delegate* unmanaged<uint, int, uint, byte, void>)&ProximityEvent;
        table.OnRpcComplete =
            (nint)(delegate* unmanaged<IntPtr, int, byte*, int, void>)&OnRpcComplete;
        table.EntityLifecycleCancel =
            (nint)(delegate* unmanaged<uint, void>)&EntityLifecycleCancel;

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
            // identical to GetEntityData's Serialize call — i.e. we go
            // through the generator-emitted ServerEntity.Serialize, NOT
            // a new SerializeFull variant.
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
    public static void DispatchRpc(uint entityId, uint rpcId, IntPtr replyChannel,
                                   byte* payload, int len)
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

            // direction lives in bits 22-23 of the *real* rpc_id; mask off
            // kReplyBit first so that bit doesn't leak into the index calc.
            uint maskedId = rpcId & 0x7FFFFFFFu;
            int direction = (int)((maskedId >> 22) & 0x3);
            if (direction == 1)
            {
                Log.Warning($"DispatchRpc: direction=1 is reserved (rpcId=0x{rpcId:X6})");
                return;
            }
            int id = (int)rpcId;

            var dispatcher = RpcBridge.Dispatchers[direction];
            dispatcher?.Invoke(entity, id, replyChannel, ref reader);
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }

    // Proximity enter/leave bridge from C++ ProximityController.
    // entityId — owner of the sensor; userArg — script's handle to
    // disambiguate multiple sensors per entity; peerEntityId — base_id of
    // the crossing peer (stable across Offload / visible client-side);
    // isEnter — 1 on inbound, 0 on outbound.
    // TODO: route to ServerEntity.OnProximityEnter/Leave once the generator
    // emits the override; for now we log so integration tests can observe
    // that the call reached managed code.
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

            // Log at Debug until OnProximityEnter/Leave is generator-emitted.
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

    [UnmanagedCallersOnly]
    public static void EntityLifecycleCancel(uint entityId)
    {
        try
        {
            ThreadGuard.EnsureMainThread();
            EntityManager.Instance.Get(entityId)?.TriggerLifecycleCancellation();
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }

    // managedHandle is returned to GCHandlePool on every completion path.
    // status mirrors Atlas.Coro.Rpc.RpcCompletionStatus.
    [UnmanagedCallersOnly]
    public static void OnRpcComplete(IntPtr managedHandle, int status, byte* payload, int len)
    {
        try
        {
            ThreadGuard.EnsureMainThread();

            if (managedHandle == IntPtr.Zero) return;
            var callback = GCHandle.FromIntPtr(managedHandle).Target as Atlas.Coro.Rpc.IAtlasRpcCallback;
            Atlas.Runtime.Coro.GCHandlePool.Return(managedHandle);
            if (callback is null)
            {
                Log.Warning("OnRpcComplete: managed handle did not resolve to a callback");
                return;
            }

            if (status == 0)
            {
                var span = len > 0 ? new ReadOnlySpan<byte>(payload, len) : ReadOnlySpan<byte>.Empty;
                callback.OnReply(span);
            }
            else
            {
                callback.OnError((Atlas.Coro.Rpc.RpcCompletionStatus)status);
            }
        }
        catch (Exception ex)
        {
            ErrorBridge.SetError(ex);
        }
    }
}
