using System;

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
    public const int kMaxSlot = 0x7F;
    public const int kMaxDirection = 0x3;
    public const int kMaxTypeIndex = 0x3FFF;
    public const int kMaxMethodIdx = 0xFF;

    public static int Encode(int slot, byte direction, ushort typeIndex, int methodIdx)
    {
        if ((uint)slot > kMaxSlot)
            throw new InvalidOperationException(
                $"RpcId slot={slot} exceeds 7-bit limit ({kMaxSlot}); too many synced components on one entity.");
        if (direction > kMaxDirection)
            throw new InvalidOperationException(
                $"RpcId direction={direction} exceeds 2-bit limit ({kMaxDirection}).");
        if (typeIndex > kMaxTypeIndex)
            throw new InvalidOperationException(
                $"RpcId typeIndex={typeIndex} exceeds 14-bit limit ({kMaxTypeIndex}); too many entity types.");
        if ((uint)methodIdx > kMaxMethodIdx)
            throw new InvalidOperationException(
                $"RpcId methodIdx={methodIdx} exceeds 8-bit limit ({kMaxMethodIdx}); too many methods per direction on one entity/component.");
        return (slot << 24) | (direction << 22) | (typeIndex << 8) | methodIdx;
    }
}
