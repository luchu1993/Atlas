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
}

internal static unsafe class NativeCallbacks
{
    private static byte[]? s_lastSerialized;
    private static GCHandle s_lastSerializedHandle;

    public static void Register()
    {
        NativeCallbackTable table;
        table.RestoreEntity =
            (nint)(delegate* unmanaged<uint, ushort, long, byte*, int, void>)&RestoreEntity;
        table.GetEntityData = (nint)(delegate* unmanaged<uint, byte**, int*, void>)&GetEntityData;
        table.EntityDestroyed = (nint)(delegate* unmanaged<uint, void>)&EntityDestroyed;

        NativeApi.SetNativeCallbacks(&table, sizeof(NativeCallbackTable));
    }

    public static void Reset()
    {
        if (s_lastSerializedHandle.IsAllocated)
        {
            s_lastSerializedHandle.Free();
        }
        s_lastSerialized = null;
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
}
