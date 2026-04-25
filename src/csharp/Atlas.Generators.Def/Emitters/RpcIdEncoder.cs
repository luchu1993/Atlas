namespace Atlas.Generators.Def.Emitters;

// Single source of truth for rpc_id wire encoding. Layout (32-bit):
//
//   bits 24-31  slot_idx       0 = entity body, >0 = component slot
//   bits 22-23  direction      0=Client, 2=Cell, 3=Base
//   bits  8-21  typeIndex      entity type (per-process typeIndexMap)
//   bits  0-7   methodIdx      1-based per direction within the entity
//
// Component RPCs share `typeIndex` with their owning entity — the entity
// is the dispatch target. `slot_idx > 0` simply tells the receive-side
// dispatcher to look up `_replicated[slot]` and call the method on the
// component instance instead of the entity body. Entity-level RPCs
// (slot=0) leave bits 24-31 zero, so their rpc_ids fit unchanged in
// MessageID (u16) for wire compatibility with component-unaware clients.
internal static class RpcIdEncoder
{
    public static int Encode(int slot, byte direction, ushort typeIndex, int methodIdx)
    {
        // No defensive bounds check — generators always pass valid values
        // and a runtime guard would just hide a real codegen bug.
        return (slot << 24) | (direction << 22) | (typeIndex << 8) | methodIdx;
    }
}
