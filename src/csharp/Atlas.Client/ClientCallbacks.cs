using System;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Serialization;

namespace Atlas.Client;

public static class ClientCallbacks
{
    public delegate void RpcDispatchDelegate(ClientEntity entity, int rpcId, ref SpanReader reader);

    public static RpcDispatchDelegate? ClientRpcDispatcher;

    // Must match src/server/baseapp/delta_forwarder.h::kClient*MessageId byte-for-byte.
    public const ushort kClientDeltaMessageId = 0xF001;
    public const ushort kClientBaselineMessageId = 0xF002;
    public const ushort kClientReliableDeltaMessageId = 0xF003;

    // Must match src/server/cellapp/cell_aoi_envelope.h::CellAoIEnvelopeKind.
    private const byte kEntityEnter = 1;
    private const byte kEntityLeave = 2;
    private const byte kEntityPositionUpdate = 3;
    private const byte kEntityPropertyUpdate = 4;

    private static readonly ClientEntityManager s_entityMgr = new();

    public static ClientEntityManager EntityManager => s_entityMgr;

    public static void DispatchRpc(uint entityId, uint rpcId, ulong traceId,
                                   ReadOnlySpan<byte> payload)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchRpc);
        using var __trace = Atlas.Diagnostics.TraceContext.BeginInbound((long)traceId);
        try
        {
            var entity = s_entityMgr.Get(entityId);
            if (entity is null) return;

            var reader = new SpanReader(payload);
            ClientRpcDispatcher?.Invoke(entity, (int)rpcId, ref reader);
        }
        catch (Exception ex)
        {
            ClientLog.Error($"DispatchRpc error: {ex}");
        }
    }

    public static void CreateEntity(uint entityId, ushort typeId)
    {
        try
        {
            var entity = ClientEntityFactory.Create(typeId);
            if (entity == null)
            {
                ClientLog.Error(
                    $"ClientCallbacks.CreateEntity: no factory registered for typeId={typeId} "
                    + $"(entityId={entityId})");
                return;
            }

            entity.EntityId = entityId;
            s_entityMgr.Register(entity);
            entity.OnInit();
        }
        catch (Exception ex)
        {
            ClientLog.Error($"CreateEntity error: {ex}");
        }
    }

    public static void DestroyEntity(uint entityId)
    {
        try
        {
            s_entityMgr.Destroy(entityId);
        }
        catch (Exception ex)
        {
            ClientLog.Error($"DestroyEntity error: {ex}");
        }
    }

    public static void DeliverFromServer(ushort msgId, ReadOnlySpan<byte> body)
    {
        try
        {
            switch (msgId)
            {
                case kClientDeltaMessageId:
                case kClientReliableDeltaMessageId:
                    DispatchAoIEnvelope(body);
                    break;
                case kClientBaselineMessageId:
                    DispatchBaseline(body);
                    break;
                default:
                    ClientLog.Error(
                        $"DeliverFromServer: unexpected msgId=0x{msgId:X4} len={body.Length}");
                    break;
            }
        }
        catch (Exception ex)
        {
            ClientLog.Error($"DeliverFromServer error (msgId=0x{msgId:X4}): {ex}");
        }
    }

    // 0xF001 / 0xF003 envelope (cell_aoi_envelope.h): [u8 kind][u32 LE eid][payload].
    private const int kEnvelopeHeaderBytes = 1 + 4;
    private static void DispatchAoIEnvelope(ReadOnlySpan<byte> body)
    {
        if (body.Length < kEnvelopeHeaderBytes)
        {
            ClientLog.Error($"DispatchAoIEnvelope: truncated envelope ({body.Length} bytes)");
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
                s_entityMgr.OnLeave(entityId);
                break;
            case kEntityPositionUpdate:
                DispatchPositionUpdate(entityId, inner);
                break;
            case kEntityPropertyUpdate:
                DispatchPropertyUpdate(entityId, inner);
                break;
            default:
                ClientLog.Error(
                    $"DispatchAoIEnvelope: unknown kind={kind} entityId={entityId}");
                break;
        }
    }

    // kEntityEnter (witness.cc::BuildEnterPayload): [u16 typeId][3f pos][3f dir][u8 onGround][peerSnapshot].
    private const int kEnterFixedBytes = 2 + 6 * 4 + 1;
    private static void DispatchEnter(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchEnter);
        if (inner.Length < kEnterFixedBytes)
        {
            ClientLog.Error($"DispatchEnter: truncated ({inner.Length} bytes)");
            return;
        }
        var reader = new SpanReader(inner);
        ushort typeId = reader.ReadUInt16();
        var pos = reader.ReadVector3();
        var dir = reader.ReadVector3();
        bool onGround = reader.ReadByte() != 0;

        var snapshot = inner.Slice(kEnterFixedBytes);
        s_entityMgr.OnEnter(entityId, typeId, pos, dir, onGround, snapshot);
    }

    // kEntityPropertyUpdate (witness.cc::BuildPropertyUpdatePayload): [u64 eventSeq][delta|snapshot].
    private const int kPropertyUpdatePrefixBytes = 8;
    private static void DispatchPropertyUpdate(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchPropertyUpdate);
        if (inner.Length < kPropertyUpdatePrefixBytes)
        {
            ClientLog.Error(
                $"DispatchPropertyUpdate: truncated ({inner.Length} bytes, need at least 8)");
            return;
        }
        var reader = new SpanReader(inner);
        ulong eventSeq = reader.ReadUInt64();
        var delta = inner.Slice(kPropertyUpdatePrefixBytes);
        s_entityMgr.ApplyPropertyDelta(entityId, eventSeq, delta);
    }

    // kEntityPositionUpdate inner payload (witness.cc::SendEntityUpdate volatile branch):
    //   [3f pos] [3f dir] [u8 on_ground]
    private const int kPositionUpdateBytes = 6 * 4 + 1;
    private static void DispatchPositionUpdate(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchPositionUpdate);
        if (inner.Length < kPositionUpdateBytes)
        {
            ClientLog.Error($"DispatchPositionUpdate: truncated ({inner.Length} bytes)");
            return;
        }
        var reader = new SpanReader(inner);
        var pos = reader.ReadVector3();
        var dir = reader.ReadVector3();
        bool onGround = reader.ReadByte() != 0;
        s_entityMgr.ApplyPosition(entityId, pos, dir, onGround);
    }

    // Wire layout for 0xF002 (baseapp_messages.h::ReplicatedBaselineToClient):
    //   [PackedInt base_entity_id] [PackedInt snapshot_size] [snapshot bytes]
    private static void DispatchBaseline(ReadOnlySpan<byte> body)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchBaseline);
        var reader = new SpanReader(body);
        uint entityId = reader.ReadPackedUInt32();
        uint size = reader.ReadPackedUInt32();
        if ((uint)reader.Remaining < size)
        {
            ClientLog.Error(
                $"DispatchBaseline: truncated snapshot (want={size} have={reader.Remaining})");
            return;
        }
        var snapshot = body.Slice(reader.Position, (int)size);
        s_entityMgr.ApplyBaseline(entityId, snapshot);
    }
}
