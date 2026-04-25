using System;
using Atlas.Serialization;

namespace Atlas.Components;

// Client-side counterpart of `Atlas.Entity.Components.ReplicatedComponent`.
//
// The client never builds outbound deltas, so this base has none of the
// server's dirty-tracking machinery. It exposes:
//   * `ApplyDelta` — overridden by codegen; consumes a per-component
//     delta written by the server's `WriteOwnerDelta` / `WriteOtherDelta`.
//   * `SendCellRpc` / `SendBaseRpc` — outbound component RPCs. Encode
//     the slot into the rpc_id (bits 24-31) so the server-side dispatcher
//     can route to the right component on the entity. ClientRpc isn't
//     emitted because clients never call client_methods on themselves.
//
// Slot index is set once at attach time by ClientEntity; instances never
// migrate slots, so caching the rpc_id base on first send would shave
// bookkeeping at a tiny memory cost — punted unless profiling demands it.
public abstract class ClientReplicatedComponent : ClientComponentBase
{
    internal int _slotIdx;
    public int SlotIdx => _slotIdx;

    /// <summary>
    /// Apply an incoming per-component delta. Wire layout mirrors the
    /// entity-level delta:
    ///   [u8 sectionMask]  bit0=scalar, bit1=container
    ///   if bit0: [u64 scalarFlags][values...]
    ///   if bit1: [u64 containerFlags][ops...]
    /// Codegen overrides per concrete component class.
    /// </summary>
    // public so an override can live in any assembly without IVT
    // plumbing. Symmetry with the server-side ReplicatedComponent.
    public virtual void ApplyDelta(ref SpanReader reader) { }

    // RPC sends compute rpc_id at runtime from (slot, direction, entity
    // type id, methodIdx). Direction + methodIdx are constants per
    // generated stub; slot and entity-type are per-instance, supplied here.
    protected void SendCellRpc(int methodIdx, ReadOnlySpan<byte> payload)
    {
        int rpcId = (_slotIdx << 24) | (0x02 << 22) | (_entity.TypeId << 8) | methodIdx;
        _entity.SendCellRpc(rpcId, payload);
    }

    protected void SendBaseRpc(int methodIdx, ReadOnlySpan<byte> payload)
    {
        int rpcId = (_slotIdx << 24) | (0x03 << 22) | (_entity.TypeId << 8) | methodIdx;
        _entity.SendBaseRpc(rpcId, payload);
    }

    // public so cross-assembly codegen can call it from the entity's
    // ApplyComponentSection (auto-attaches missing components). The
    // double-underscore prefix marks it as runtime-only — scripts never
    // call it directly.
    public void __Bind(Atlas.Client.ClientEntity entity, int slotIdx)
    {
        _entity = entity;
        _slotIdx = slotIdx;
    }
}
