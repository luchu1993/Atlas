using System;
using System.Collections.Generic;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Serialization;

namespace Atlas.Client;

public sealed class ClientSession
{
    private const byte kEntityEnter = 1;
    private const byte kEntityLeave = 2;
    private const byte kEntityPositionUpdate = 3;
    private const byte kEntityPropertyUpdate = 4;
    private const byte kSpaceDataInit = 5;
    private const byte kSpaceDataUpdate = 6;
    private const byte kSpaceDataDelete = 7;
    private const int kEnvelopeHeaderBytes = 1 + 4;
    private const int kEnterFixedBytes = 2 + 6 * 4 + 1 + 8;
    private const int kPropertyUpdatePrefixBytes = 8;
    private const int kPositionUpdateBytes = 6 * 4 + 1 + 8;
    private const int kClientRpcPrefixBytes = 4 + 4 + 8;
    private const int kEntityTransferredBytes = 4 + 2;

    public ClientSession()
    {
        EntityFactory = new ClientEntityFactoryRegistry();
        EntityManager = new ClientEntityManager(EntityFactory, this);
        SpaceDataManager = new SpaceDataManager();
    }

    public ClientEntityFactoryRegistry EntityFactory { get; }
    public ClientEntityManager EntityManager { get; }
    public SpaceDataManager SpaceDataManager { get; }
    public ClientCallbacks.RpcDispatchDelegate? ClientRpcDispatcher { get; set; }
    public ClientHost.SendRpcFn? SendBaseRpcHandler { get; set; }
    public ClientHost.SendRpcFn? SendCellRpcHandler { get; set; }
    public ClientHost.ReportEventSeqGapFn? ReportEventSeqGapHandler { get; set; }

    public event Action<uint, IReadOnlyList<ClientCallbacks.BspLeafRect>>? SpaceBspGeometryReceived;

    public void Reset()
    {
        EntityManager.Clear();
        SpaceDataManager.Clear();
    }

    public void Tick(float deltaTime)
    {
        EntityManager.TickInterpolation(deltaTime);
    }

    public void DispatchRpc(uint entityId, uint rpcId, ulong traceId,
                            ReadOnlySpan<byte> payload)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchRpc);
        using var __trace = Atlas.Diagnostics.TraceContext.BeginInbound((long)traceId);
        try
        {
            var entity = EntityManager.Get(entityId);
            if (entity is null) return;

            var reader = new SpanReader(payload);
            ClientRpcDispatcher?.Invoke(entity, (int)rpcId, ref reader);
        }
        catch (Exception ex)
        {
            Log.Error($"DispatchRpc error: {ex}");
        }
    }

    internal void SendBaseRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                              ulong traceId)
    {
        Required(SendBaseRpcHandler, nameof(SendBaseRpcHandler))(
            entityId, rpcId, payload, traceId);
    }

    internal void SendCellRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload,
                              ulong traceId)
    {
        Required(SendCellRpcHandler, nameof(SendCellRpcHandler))(
            entityId, rpcId, payload, traceId);
    }

    internal void ReportEventSeqGap(uint entityId, uint gapDelta)
    {
        ReportEventSeqGapHandler?.Invoke(entityId, gapDelta);
    }

    public void CreateEntity(uint entityId, ushort typeId)
    {
        try
        {
            var entity = EntityFactory.Create(typeId);
            if (entity == null)
            {
                Log.Error(
                    $"ClientSession.CreateEntity: no factory registered for typeId={typeId} "
                    + $"(entityId={entityId})");
                return;
            }

            entity.EntityId = entityId;
            entity.IsOwner = true;
            entity.Session = this;
            EntityManager.Register(entity, publish: false);
            entity.OnInit();
            EntityManager.PublishEntityAdded(entity);
        }
        catch (Exception ex)
        {
            Log.Error($"CreateEntity error: {ex}");
        }
    }

    public void DestroyEntity(uint entityId)
    {
        try
        {
            EntityManager.Destroy(entityId);
        }
        catch (Exception ex)
        {
            Log.Error($"DestroyEntity error: {ex}");
        }
    }

    public void DeliverFromServer(ushort msgId, ReadOnlySpan<byte> body)
    {
        try
        {
            switch (msgId)
            {
                case ClientCallbacks.kClientDeltaMessageId:
                case ClientCallbacks.kClientReliableDeltaMessageId:
                    DispatchAoIEnvelope(body);
                    break;
                case ClientCallbacks.kClientBaselineMessageId:
                    DispatchBaseline(body);
                    break;
                case ClientCallbacks.kEntityTransferredMessageId:
                    DispatchEntityTransferred(body);
                    break;
                case ClientCallbacks.kCellReadyMessageId:
                    break;
                case ClientCallbacks.kClientRpcMessageId:
                    DispatchClientRpc(body);
                    break;
                case ClientCallbacks.kSpaceBspGeometryMessageId:
                    DispatchSpaceBspGeometry(body);
                    break;
                default:
                    Log.Error(
                        $"DeliverFromServer: unexpected msgId=0x{msgId:X4} len={body.Length}");
                    break;
            }
        }
        catch (Exception ex)
        {
            Log.Error($"DeliverFromServer error (msgId=0x{msgId:X4}): {ex}");
        }
    }

    private void DispatchAoIEnvelope(ReadOnlySpan<byte> body)
    {
        if (body.Length < kEnvelopeHeaderBytes)
        {
            Log.Error($"DispatchAoIEnvelope: truncated envelope ({body.Length} bytes)");
            return;
        }
        var reader = new SpanReader(body);
        byte kind = reader.ReadByte();
        uint entityId = reader.ReadUInt32();
        var inner = body.Slice(kEnvelopeHeaderBytes);

        switch (kind)
        {
            case kEntityEnter:
                DispatchEnter(entityId, inner);
                break;
            case kEntityLeave:
                EntityManager.OnLeave(entityId);
                break;
            case kEntityPositionUpdate:
                DispatchPositionUpdate(entityId, inner);
                break;
            case kEntityPropertyUpdate:
                DispatchPropertyUpdate(entityId, inner);
                break;
            case kSpaceDataInit:
                DispatchSpaceDataInit(entityId, inner);
                break;
            case kSpaceDataUpdate:
                DispatchSpaceDataUpdate(entityId, inner);
                break;
            case kSpaceDataDelete:
                DispatchSpaceDataDelete(entityId, inner);
                break;
            default:
                Log.Error(
                    $"DispatchAoIEnvelope: unknown kind={kind} entityId={entityId}");
                break;
        }
    }

    private void DispatchSpaceDataInit(uint spaceId, ReadOnlySpan<byte> inner)
    {
        if (inner.Length < 4)
        {
            Log.Error($"DispatchSpaceDataInit: truncated ({inner.Length} bytes)");
            return;
        }
        var reader = new SpanReader(inner);
        uint count = reader.ReadUInt32();
        var entries = new List<(ushort, byte[])>((int)count);
        for (uint i = 0; i < count; ++i)
        {
            if (reader.Remaining < 2 + 4)
            {
                Log.Error($"DispatchSpaceDataInit: truncated entry header at i={i}");
                return;
            }
            ushort keyId = reader.ReadUInt16();
            uint vlen = reader.ReadUInt32();
            if ((uint)reader.Remaining < vlen)
            {
                Log.Error(
                    $"DispatchSpaceDataInit: truncated value (want={vlen} have={reader.Remaining})");
                return;
            }
            var bytes = inner.Slice(reader.Position, (int)vlen).ToArray();
            reader.Advance((int)vlen);
            entries.Add((keyId, bytes));
        }
        SpaceDataManager.InitSpace(spaceId, entries);
    }

    private void DispatchSpaceDataUpdate(uint spaceId, ReadOnlySpan<byte> inner)
    {
        if (inner.Length < 2 + 4)
        {
            Log.Error($"DispatchSpaceDataUpdate: truncated ({inner.Length} bytes)");
            return;
        }
        var reader = new SpanReader(inner);
        ushort keyId = reader.ReadUInt16();
        uint vlen = reader.ReadUInt32();
        if ((uint)reader.Remaining < vlen)
        {
            Log.Error($"DispatchSpaceDataUpdate: truncated value");
            return;
        }
        var bytes = inner.Slice(reader.Position, (int)vlen).ToArray();
        SpaceDataManager.SetKey(spaceId, keyId, bytes);
    }

    private void DispatchSpaceDataDelete(uint spaceId, ReadOnlySpan<byte> inner)
    {
        if (inner.Length < 2)
        {
            Log.Error($"DispatchSpaceDataDelete: truncated ({inner.Length} bytes)");
            return;
        }
        var reader = new SpanReader(inner);
        ushort keyId = reader.ReadUInt16();
        SpaceDataManager.RemoveKey(spaceId, keyId);
    }

    private void DispatchEnter(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchEnter);
        if (inner.Length < kEnterFixedBytes)
        {
            Log.Error($"DispatchEnter: truncated ({inner.Length} bytes)");
            return;
        }
        var reader = new SpanReader(inner);
        ushort typeId = reader.ReadUInt16();
        var pos = reader.ReadVector3();
        var dir = reader.ReadVector3();
        bool onGround = reader.ReadByte() != 0;
        double serverTime = reader.ReadDouble();

        var snapshot = inner.Slice(kEnterFixedBytes);
        EntityManager.OnEnter(entityId, typeId, serverTime, pos, dir, onGround, snapshot);
    }

    private void DispatchPropertyUpdate(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchPropertyUpdate);
        if (inner.Length < kPropertyUpdatePrefixBytes)
        {
            Log.Error(
                $"DispatchPropertyUpdate: truncated ({inner.Length} bytes, need at least 8)");
            return;
        }
        var reader = new SpanReader(inner);
        ulong eventSeq = reader.ReadUInt64();
        var delta = inner.Slice(kPropertyUpdatePrefixBytes);
        EntityManager.ApplyPropertyDelta(entityId, eventSeq, delta);
    }

    private void DispatchPositionUpdate(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchPositionUpdate);
        if (inner.Length < kPositionUpdateBytes)
        {
            Log.Error($"DispatchPositionUpdate: truncated ({inner.Length} bytes)");
            return;
        }
        var reader = new SpanReader(inner);
        var pos = reader.ReadVector3();
        var dir = reader.ReadVector3();
        bool onGround = reader.ReadByte() != 0;
        double serverTime = reader.ReadDouble();
        EntityManager.ApplyPosition(entityId, serverTime, pos, dir, onGround);
    }

    private void DispatchClientRpc(ReadOnlySpan<byte> body)
    {
        if (body.Length < kClientRpcPrefixBytes)
        {
            Log.Error($"DispatchClientRpc: truncated ({body.Length} bytes, need >= 16)");
            return;
        }
        var reader = new SpanReader(body);
        uint entityId = reader.ReadUInt32();
        uint rpcId = reader.ReadUInt32();
        ulong traceId = reader.ReadUInt64();
        var args = body.Slice(kClientRpcPrefixBytes);
        DispatchRpc(entityId, rpcId, traceId, args);
    }

    private void DispatchSpaceBspGeometry(ReadOnlySpan<byte> body)
    {
        var reader = new SpanReader(body);
        uint spaceId = reader.ReadUInt32();
        uint count = reader.ReadPackedUInt32();
        var leaves = new List<ClientCallbacks.BspLeafRect>((int)count);
        for (uint i = 0; i < count; i++)
        {
            uint cellId = reader.ReadUInt32();
            byte ownerIndex = reader.ReadByte();
            float minX = reader.ReadFloat();
            float minZ = reader.ReadFloat();
            float maxX = reader.ReadFloat();
            float maxZ = reader.ReadFloat();
            leaves.Add(new ClientCallbacks.BspLeafRect(cellId, ownerIndex, minX, minZ, maxX,
                                                       maxZ));
        }
        SpaceBspGeometryReceived?.Invoke(spaceId, leaves);
    }

    private void DispatchEntityTransferred(ReadOnlySpan<byte> body)
    {
        if (body.Length < kEntityTransferredBytes)
        {
            Log.Error($"DispatchEntityTransferred: truncated ({body.Length} bytes, need 6)");
            return;
        }
        var reader = new SpanReader(body);
        uint newEntityId = reader.ReadUInt32();
        ushort newTypeId = reader.ReadUInt16();
        CreateEntity(newEntityId, newTypeId);
    }

    private void DispatchBaseline(ReadOnlySpan<byte> body)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchBaseline);
        var reader = new SpanReader(body);
        uint entityId = reader.ReadPackedUInt32();
        uint size = reader.ReadPackedUInt32();
        if ((uint)reader.Remaining < size)
        {
            Log.Error(
                $"DispatchBaseline: truncated snapshot (want={size} have={reader.Remaining})");
            return;
        }
        var snapshot = body.Slice(reader.Position, (int)size);
        EntityManager.ApplyBaseline(entityId, snapshot);
    }

    private static T Required<T>(T? handler, string name) where T : Delegate
        => handler ?? throw new InvalidOperationException(
            $"ClientSession.{name} is not set for this session.");
}
