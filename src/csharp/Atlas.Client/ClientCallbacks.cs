using System;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Serialization;

namespace Atlas.Client;

/// <summary>
/// Pure-managed surface of the client callback layer. The host app (desktop
/// CoreCLR or Unity IL2CPP) owns the native-side callback table and routes
/// incoming wire bytes into the static <c>Dispatch*</c> decoders below.
/// </summary>
/// <remarks>
/// Atlas.Client targets netstandard2.1 for Unity consumption. Everything
/// that requires .NET 5+ primitives — [UnmanagedCallersOnly] bridges, raw
/// delegate* unmanaged, P/Invoke to a specific native dll — lives in
/// Atlas.Client.Desktop (CoreCLR) or the Unity package (Mono / IL2CPP).
/// Both hosts call into the same decoders here so wire-format parsing is
/// defined exactly once.
/// </remarks>
public static class ClientCallbacks
{
    /// <summary>
    /// Delegate type for dispatching an incoming ClientRpc to a managed entity.
    /// </summary>
    public delegate void RpcDispatchDelegate(ClientEntity entity, int rpcId, ref SpanReader reader);

    /// <summary>
    /// RPC dispatcher set by generated code (<c>[ModuleInitializer]</c>).
    /// </summary>
    public static RpcDispatchDelegate? ClientRpcDispatcher;

    // Reserved client-facing MessageIDs — must match
    // src/server/baseapp/delta_forwarder.h::kClient*MessageId exactly.
    // Duplicated here (rather than imported) because Atlas.Client must
    // remain independent of any server header.
    public const ushort kClientDeltaMessageId = 0xF001;          // volatile / unreliable
    public const ushort kClientBaselineMessageId = 0xF002;       // owner-scope full baseline (reliable)
    public const ushort kClientReliableDeltaMessageId = 0xF003;  // ordered property delta (reliable)

    // CellAoIEnvelopeKind — must match src/server/cellapp/cell_aoi_envelope.h
    // byte-for-byte.
    private const byte kEntityEnter = 1;
    private const byte kEntityLeave = 2;
    private const byte kEntityPositionUpdate = 3;
    private const byte kEntityPropertyUpdate = 4;

    private static readonly ClientEntityManager s_entityMgr = new();

    public static ClientEntityManager EntityManager => s_entityMgr;

    // =========================================================================
    // Host entry points — called by native-callback glue in the host app
    // (Atlas.Client.Desktop / Unity package). Signatures take pre-materialised
    // ReadOnlySpan<byte> so the unmanaged-to-managed conversion stays in the
    // host-specific assembly and Atlas.Client keeps a runtime-agnostic ABI.
    // =========================================================================

    /// <summary>
    /// Dispatch an incoming ClientRpc to the current entity. The host app
    /// resolves the native-side pointer / length into a ReadOnlySpan before
    /// calling through.
    /// </summary>
    public static void DispatchRpc(uint entityId, uint rpcId, ReadOnlySpan<byte> payload)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchRpc);
        try
        {
            var entity = s_entityMgr.Get(entityId);
            if (entity is null) return;

            var reader = new SpanReader(payload);
            ClientRpcDispatcher?.Invoke(entity, (int)rpcId, ref reader);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"DispatchRpc error: {ex}");
        }
    }

    /// <summary>
    /// Create the client-side instance for the local player's own entity
    /// (called once during desktop-client Authenticate flow; Unity's
    /// equivalent path differs).
    /// </summary>
    public static void CreateEntity(uint entityId, ushort typeId)
    {
        try
        {
            var entity = ClientEntityFactory.Create(typeId);
            if (entity == null)
            {
                Console.Error.WriteLine(
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
            Console.Error.WriteLine($"CreateEntity error: {ex}");
        }
    }

    /// <summary>Tear down the local-player entity (logout or forced DC).</summary>
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
    /// Entry point for the three reserved state-replication MessageIDs
    /// (<c>0xF001</c> unreliable delta / <c>0xF002</c> baseline /
    /// <c>0xF003</c> reliable delta). Host delivers the already-extracted
    /// payload; this method owns all envelope decoding.
    /// </summary>
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
                    Console.Error.WriteLine(
                        $"DeliverFromServer: unexpected msgId=0x{msgId:X4} len={body.Length}");
                    break;
            }
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"DeliverFromServer error (msgId=0x{msgId:X4}): {ex}");
        }
    }

    // =========================================================================
    // Envelope decoders
    // =========================================================================

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
    // for AoI peers.
    private const int kEnterFixedBytes = 2 + 6 * 4 + 1;
    private static void DispatchEnter(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchEnter);
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
    // The seq prefix lets the client detect missing reliable deltas. It is
    // authoritative on the delta channel (ordered, nominally gap-free) and
    // approximately correct on snapshot fallback (reflects state up to
    // latest_event_seq).
    private const int kPropertyUpdatePrefixBytes = 8;
    private static void DispatchPropertyUpdate(uint entityId, ReadOnlySpan<byte> inner)
    {
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchPropertyUpdate);
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
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchPositionUpdate);
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
        using var _ = Profiler.ZoneN(ProfilerNames.ClientDispatchBaseline);
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
