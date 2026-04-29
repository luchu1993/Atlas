using Atlas.Core;
using Atlas.Serialization;

namespace Atlas.Entity.Components;

// Synced Component base — every .def-declared component without
// `local="..."` derives from this. Slot index is fixed at type
// registration; the dirty bit per property folds into the entity's
// _dirtyComponents bitmap so the replication pump can skip clean
// components without scanning each one.
public abstract class ReplicatedComponent : ComponentBase
{
    internal int _slotIdx;

    // Slot the entity assigned at AddComponent time. Stable for the
    // lifetime of the component instance.
    public int SlotIdx => _slotIdx;

    // Per-component property dirty bitmap. Codegen-emitted subclasses
    // index it by their declared properties; this base keeps it ulong
    // so up to 64 properties per component are free. protected (not
    // private protected) so derived classes in any assembly — including
    // the user-script assemblies that consume the codegen — can read it
    // from the generated WriteOwnerDelta override.
    protected ulong _dirtyFlags;

    // Marks property `propIdx` dirty AND notifies the owning entity
    // that this component slot has pending state. The dual-tier dirty
    // (per-prop within component, per-component within entity) keeps
    // BuildAndConsume's fast path free when nothing changed.
    protected void MarkDirty(int propIdx)
    {
        _dirtyFlags |= 1UL << propIdx;
        _entity.__MarkComponentDirty(_slotIdx);
    }

    public bool IsDirty => _dirtyFlags != 0;
    public void ClearDirty() => _dirtyFlags = 0;

    // RPC send helpers — compose rpc_id at runtime from (slot, direction,
    // entity TypeId, methodIdx). Direction + methodIdx are constants per
    // generated stub method; slot and TypeId are per-instance, so a single
    // component class instance can be slotted onto different entity types
    // and still target the right server-side dispatcher table.
    protected void SendClientRpc(int methodIdx, RpcTarget target,
        System.ReadOnlySpan<byte> payload)
    {
        int rpcId = (_slotIdx << 24) | (0x00 << 22) | (_entity.TypeId << 8) | methodIdx;
        _entity.SendClientRpc(rpcId, target, payload);
    }

    protected void SendCellRpc(int methodIdx, System.ReadOnlySpan<byte> payload)
    {
        int rpcId = (_slotIdx << 24) | (0x02 << 22) | (_entity.TypeId << 8) | methodIdx;
        _entity.SendCellRpc(rpcId, payload);
    }

    protected void SendBaseRpc(int methodIdx, System.ReadOnlySpan<byte> payload)
    {
        int rpcId = (_slotIdx << 24) | (0x03 << 22) | (_entity.TypeId << 8) | methodIdx;
        _entity.SendBaseRpc(rpcId, payload);
    }

    // Generator-emitted subclasses override; the base no-op makes
    // hand-written test components valid without a frame implementation.
    public virtual bool BuildFrame(
        ref SpanWriter ownerSnapshot, ref SpanWriter otherSnapshot,
        ref SpanWriter ownerDelta, ref SpanWriter otherDelta) => false;

    // Per-audience predicates. The entity-side replication pump uses
    // these to decide whether to set sectionMask bit 2 ("components
    // present") on each audience channel without scanning every dirty
    // flag twice. Default false keeps hand-written components inert.
    // public so codegen-emitted overrides + entity-side accessor partials
    // can live in any assembly without IVT plumbing.
    public virtual bool HasOwnerDirty() => false;
    public virtual bool HasOtherDirty() => false;

    // Audience-filtered per-component delta. Layout:
    //   [u8 sectionMask]  bit0 = scalar, bit1 = container
    //   if bit0: [flags][scalar values...]
    //   if bit1: [flags][container ops...]
    // Mirrors the entity-level audience delta wire format so the
    // per-component decoder can reuse the same parser shape.
    public virtual void WriteOwnerDelta(ref SpanWriter writer) { }
    public virtual void WriteOtherDelta(ref SpanWriter writer) { }

    internal void __Bind(ServerEntity entity, int slotIdx)
    {
        _entity = entity;
        _slotIdx = slotIdx;
    }
}
