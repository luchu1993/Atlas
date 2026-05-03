using System;
using Atlas.Generators.Def.Emitters;
using Xunit;

namespace Atlas.Generators.Tests;

public class RpcIdEncoderTests
{
    [Fact]
    public void Encode_PacksFieldsIntoExpectedBits()
    {
        int id = RpcIdEncoder.Encode(slot: 0, direction: 0x02, typeIndex: 0x1234, methodIdx: 0x05);
        Assert.Equal(0x00 << 24 | 0x02 << 22 | 0x1234 << 8 | 0x05, id);
    }

    [Fact]
    public void Encode_SlotOverflow_Throws()
        => Assert.Throws<InvalidOperationException>(
            () => RpcIdEncoder.Encode(slot: 0x80, direction: 0, typeIndex: 1, methodIdx: 1));

    [Fact]
    public void Encode_DirectionOverflow_Throws()
        => Assert.Throws<InvalidOperationException>(
            () => RpcIdEncoder.Encode(slot: 0, direction: 0x4, typeIndex: 1, methodIdx: 1));

    [Fact]
    public void Encode_TypeIndexOverflow_Throws()
        => Assert.Throws<InvalidOperationException>(
            () => RpcIdEncoder.Encode(slot: 0, direction: 0, typeIndex: 0x4000, methodIdx: 1));

    [Fact]
    public void Encode_MethodIdxOverflow_Throws()
        => Assert.Throws<InvalidOperationException>(
            () => RpcIdEncoder.Encode(slot: 0, direction: 0, typeIndex: 1, methodIdx: 0x100));

    [Fact]
    public void Encode_AcceptsAllFieldsAtMaxValue()
    {
        int id = RpcIdEncoder.Encode(
            slot: RpcIdEncoder.kMaxSlot,
            direction: (byte)RpcIdEncoder.kMaxDirection,
            typeIndex: (ushort)RpcIdEncoder.kMaxTypeIndex,
            methodIdx: RpcIdEncoder.kMaxMethodIdx);
        Assert.Equal(unchecked((int)0x7FFF_FFFF), id);
    }
}
