namespace Atlas.Generators.Def.Emitters;

// rpc_id wire layout (32-bit):
//   bit  31      kReplyBit       1 = sender awaits an EntityRpcReply
//   bits 24-30   slot_idx        0 = entity body, >0 = component slot
//   bits 22-23   direction       0=Client, 2=Cell, 3=Base
//   bits  8-21   typeIndex
//   bits  0-7    methodIdx
internal static class RpcIdEncoder
{
    public const uint kReplyBit = 1u << 31;

    public static int Encode(int slot, byte direction, ushort typeIndex, int methodIdx)
    {
        // slot is 7 bits now; component count was already capped well below 128.
        return (slot << 24) | (direction << 22) | (typeIndex << 8) | methodIdx;
    }
}
