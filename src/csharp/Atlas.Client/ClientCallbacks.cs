using System;
using System.Runtime.InteropServices;
using Atlas.Serialization;

namespace Atlas.Client;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal unsafe struct ClientCallbackTable
{
    public nint DispatchRpc;
    public nint CreateEntity;
    public nint DestroyEntity;
}

/// <summary>
/// Manages the callback table from C++ → C# on the client side.
/// </summary>
public static unsafe class ClientCallbacks
{
    /// <summary>
    /// Delegate type for dispatching an incoming ClientRpc to a managed entity.
    /// </summary>
    public delegate void RpcDispatchDelegate(ClientEntity entity, int rpcId, ref SpanReader reader);

    /// <summary>
    /// RPC dispatcher set by generated code.
    /// Index: 0 = ClientRpc (the only direction the client receives)
    /// </summary>
    public static RpcDispatchDelegate? ClientRpcDispatcher;

    private static readonly ClientEntityManager s_entityMgr = new();

    public static ClientEntityManager EntityManager => s_entityMgr;

    internal static void Register()
    {
        ClientCallbackTable table;
        table.DispatchRpc = (nint)(delegate* unmanaged<uint, uint, byte*, int, void>)&DispatchRpc;
        table.CreateEntity = (nint)(delegate* unmanaged<uint, ushort, void>)&CreateEntity;
        table.DestroyEntity = (nint)(delegate* unmanaged<uint, void>)&DestroyEntity;

        ClientNativeApi.SetNativeCallbacks(&table, sizeof(ClientCallbackTable));
    }

    [UnmanagedCallersOnly]
    public static void DispatchRpc(uint entityId, uint rpcId, byte* payload, int len)
    {
        try
        {
            var entity = s_entityMgr.Get(entityId);
            if (entity is null) return;

            var reader = new SpanReader(new ReadOnlySpan<byte>(payload, len));
            int id = (int)rpcId;

            ClientRpcDispatcher?.Invoke(entity, id, ref reader);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"DispatchRpc error: {ex}");
        }
    }

    [UnmanagedCallersOnly]
    public static void CreateEntity(uint entityId, ushort typeId)
    {
        try
        {
            var entity = ClientEntityFactory.Create(typeId);
            if (entity == null) return;

            entity.EntityId = entityId;
            s_entityMgr.Register(entity);
            entity.OnInit();
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"CreateEntity error: {ex}");
        }
    }

    [UnmanagedCallersOnly]
    public static void DestroyEntity(uint entityId)
    {
        try
        {
            s_entityMgr.Destroy(entityId);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"DestroyEntity error: {ex}");
        }
    }
}
