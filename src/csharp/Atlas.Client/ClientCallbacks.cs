using System;
using System.Runtime.InteropServices;
using Atlas.DataTypes;
using Atlas.Serialization;

namespace Atlas.Client;

[StructLayout(LayoutKind.Sequential, Pack = 1)]
internal unsafe struct ClientCallbackTable
{
    public nint DispatchRpc;
    public nint CreateEntity;
    public nint DestroyEntity;
    public nint DeliverFromServer;
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

    // Reserved client-facing MessageIDs — must match
    // src/server/baseapp/delta_forwarder.h::kClient*MessageId exactly.
    // Duplicated here (rather than imported) because Atlas.Client must remain
    // independent of any server header.
    internal const ushort kClientDeltaMessageId = 0xF001;          // volatile / unreliable
    internal const ushort kClientBaselineMessageId = 0xF002;       // owner-scope full baseline (reliable)
    internal const ushort kClientReliableDeltaMessageId = 0xF003;  // ordered property delta (reliable)

    // CellAoIEnvelopeKind — must match src/server/cellapp/cell_aoi_envelope.h
    // byte-for-byte.
    private const byte kEntityEnter = 1;
    private const byte kEntityLeave = 2;
    private const byte kEntityPositionUpdate = 3;
    private const byte kEntityPropertyUpdate = 4;

    private static readonly ClientEntityManager s_entityMgr = new();

    public static ClientEntityManager EntityManager => s_entityMgr;

    internal static void Register()
    {
        ClientCallbackTable table;
        table.DispatchRpc = (nint)(delegate* unmanaged<uint, uint, byte*, int, void>)&DispatchRpc;
        table.CreateEntity = (nint)(delegate* unmanaged<uint, ushort, void>)&CreateEntity;
        table.DestroyEntity = (nint)(delegate* unmanaged<uint, void>)&DestroyEntity;
        table.DeliverFromServer =
            (nint)(delegate* unmanaged<ushort, byte*, int, void>)&DeliverFromServer;

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

    /// <summary>
    /// Opaque transport-channel delivery hook called by the native client for
    /// the three reserved state-replication MessageIDs. Owning all envelope
    /// decoding here (rather than in C++) keeps the native layer free of
    /// property-sync business logic so a non-C# script host can bind the same
    /// hook unchanged.
    /// </summary>
    [UnmanagedCallersOnly]
    public static void DeliverFromServer(ushort msgId, byte* payload, int len)
    {
        try
        {
            var body = new ReadOnlySpan<byte>(payload, len);
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
                    Console.Error.WriteLine(
                        $"DeliverFromServer: unexpected msgId=0x{msgId:X4} len={len}");
                    break;
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"DeliverFromServer error (msgId=0x{msgId:X4}): {ex}");
        }
    }

    // -- Envelope decoders --------------------------------------------------

    // Wire layout for 0xF001 / 0xF003 (CellAoIEnvelope, src/server/cellapp/cell_aoi_envelope.h):
    //   [u8 kind] [u32 LE public_entity_id] [payload bytes...]
    private const int kEnvelopeHeaderBytes = 1 + 4;
    private static void DispatchAoIEnvelope(ReadOnlySpan<byte> body)
    {
        if (body.Length < kEnvelopeHeaderBytes)
        {
            Console.Error.WriteLine($"DispatchAoIEnvelope: truncated envelope ({body.Length} bytes)");
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
                Console.Error.WriteLine(
                    $"DispatchAoIEnvelope: unknown kind={kind} entityId={entityId}");
                break;
        }
    }

    // kEntityEnter inner payload (witness.cc::BuildEnterPayload):
    //   [u16 type_id] [3f pos] [3f dir] [u8 on_ground] [... peer_snapshot ...]
    // The peer_snapshot tail is scope-subset bytes (SerializeForOtherClients)
    // for AoI peers — ClientEntityManager gates its application on Phase B0.
    private const int kEnterFixedBytes = 2 + 6 * 4 + 1;
    private static void DispatchEnter(uint entityId, ReadOnlySpan<byte> inner)
    {
        if (inner.Length < kEnterFixedBytes)
        {
            Console.Error.WriteLine($"DispatchEnter: truncated ({inner.Length} bytes)");
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

    // kEntityPropertyUpdate inner payload (witness.cc::BuildPropertyUpdatePayload):
    //   [u64 event_seq] [delta or snapshot bytes]
    // The seq prefix lets the client notice missing reliable deltas (Phase
    // D2'.2). It is authoritative on the delta channel (ordered, gap-free
    // in the nominal case) and approximately correct on the snapshot
    // fallback (the snapshot reflects state up to latest_event_seq).
    private const int kPropertyUpdatePrefixBytes = 8;
    private static void DispatchPropertyUpdate(uint entityId, ReadOnlySpan<byte> inner)
    {
        if (inner.Length < kPropertyUpdatePrefixBytes)
        {
            Console.Error.WriteLine(
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
        if (inner.Length < kPositionUpdateBytes)
        {
            Console.Error.WriteLine($"DispatchPositionUpdate: truncated ({inner.Length} bytes)");
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
        var reader = new SpanReader(body);
        uint entityId = reader.ReadPackedUInt32();
        uint size = reader.ReadPackedUInt32();
        if ((uint)reader.Remaining < size)
        {
            Console.Error.WriteLine(
                $"DispatchBaseline: truncated snapshot (want={size} have={reader.Remaining})");
            return;
        }
        var snapshot = body.Slice(reader.Position, (int)size);
        s_entityMgr.ApplyBaseline(entityId, snapshot);
    }
}
