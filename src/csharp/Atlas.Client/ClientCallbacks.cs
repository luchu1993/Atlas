using System;
using System.Collections.Generic;
using Atlas.Serialization;

namespace Atlas.Client;

public static class ClientCallbacks
{
    public delegate void RpcDispatchDelegate(ClientEntity entity, int rpcId, ref SpanReader reader);

    public const ushort kClientDeltaMessageId = 0xF001;
    public const ushort kClientBaselineMessageId = 0xF002;
    public const ushort kClientReliableDeltaMessageId = 0xF003;
    public const ushort kEntityTransferredMessageId = 2024;
    public const ushort kCellReadyMessageId = 2025;
    public const ushort kClientRpcMessageId = 0xF004;
    public const ushort kSpaceBspGeometryMessageId = 2032;

    public static ClientSession DefaultSession { get; } = new();

    public static RpcDispatchDelegate? ClientRpcDispatcher
    {
        get => DefaultSession.ClientRpcDispatcher;
        set => DefaultSession.ClientRpcDispatcher = value;
    }

    public static ClientEntityManager EntityManager => DefaultSession.EntityManager;
    public static SpaceDataManager SpaceDataManager => DefaultSession.SpaceDataManager;

    public static event Action<uint, IReadOnlyList<BspLeafRect>>? SpaceBspGeometryReceived
    {
        add => DefaultSession.SpaceBspGeometryReceived += value;
        remove => DefaultSession.SpaceBspGeometryReceived -= value;
    }

    public static void ResetSession()
    {
        DefaultSession.Reset();
    }

    public static void DispatchRpc(uint entityId, uint rpcId, ulong traceId,
                                   ReadOnlySpan<byte> payload)
    {
        DefaultSession.DispatchRpc(entityId, rpcId, traceId, payload);
    }

    public static void CreateEntity(uint entityId, ushort typeId)
    {
        DefaultSession.CreateEntity(entityId, typeId);
    }

    public static void DestroyEntity(uint entityId)
    {
        DefaultSession.DestroyEntity(entityId);
    }

    public static void DeliverFromServer(ushort msgId, ReadOnlySpan<byte> body)
    {
        DefaultSession.DeliverFromServer(msgId, body);
    }

    public readonly struct BspLeafRect
    {
        public BspLeafRect(uint cellId, byte ownerIndex, float minX, float minZ, float maxX,
                           float maxZ, float load, uint entityCount)
        {
            CellId = cellId;
            OwnerIndex = ownerIndex;
            MinX = minX;
            MinZ = minZ;
            MaxX = maxX;
            MaxZ = maxZ;
            Load = load;
            EntityCount = entityCount;
        }

        public uint CellId { get; }
        public byte OwnerIndex { get; }
        public float MinX { get; }
        public float MinZ { get; }
        public float MaxX { get; }
        public float MaxZ { get; }
        public float Load { get; }
        public uint EntityCount { get; }
    }
}
