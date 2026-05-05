using System;
using Atlas.Serialization;

namespace Atlas.Components;

// Client counterpart of server ReplicatedComponent — no dirty tracking; client never sends deltas.
public abstract class ClientReplicatedComponent : ClientComponentBase
{
    internal int _slotIdx;
    public int SlotIdx => _slotIdx;

    // Wire: [u8 sectionMask] [if bit0: u64 scalarFlags + values] [if bit1: u64 containerFlags + ops].
    // public for cross-assembly codegen overrides without IVT plumbing.
    public virtual void ApplyDelta(ref SpanReader reader) { }

    // rpc_id = slot<<24 | dir<<22 | typeId<<8 | methodIdx. Slot + typeId are per-instance.
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

    // public for cross-assembly codegen; double-underscore marks runtime-only — scripts never call it.
    public void __Bind(Atlas.Client.ClientEntity entity, int slotIdx)
    {
        _entity = entity;
        _slotIdx = slotIdx;
    }
}
