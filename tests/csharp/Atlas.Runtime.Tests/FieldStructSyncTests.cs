using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

[Entity("FieldStructTestEntity")]
public partial class FieldStructTestEntity : ServerEntity
{
}

// list<StatsBlock> where StatsBlock is sync="field". Single-field
// mutation goes through ItemAt and emits kStructFieldSet — the wire
// carries one field's bytes instead of the whole 32-byte body.
public class FieldStructSyncTests
{
    [Fact]
    public void SingleFieldMutation_RoundTripsAndShipsOnlyChangedField()
    {
        var src = new FieldStructTestEntity();
        var dst = new FieldStructTestEntity();
        src.Party.Add(new Atlas.Def.StatsBlock
        {
            Hp = 100, Mp = 50, Atk = 10, Def = 5,
            Agi = 7, Luck = 3, Vit = 8, Ele = 2,
        });
        ApplyOneFrame(src, dst);
        Assert.Equal(100, dst.Party[0].Hp);

        // Mutate one field through the ItemAt accessor — the wire delta
        // for this tick should be a kStructFieldSet, not a full kSet.
        src.PartyAt(0).Hp = 999;

        var buf = FrameBufferSet.Rent();
        byte[] bytes;
        try
        {
            Assert.True(src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _));
            bytes = buf.OwnerDelta.WrittenSpan.ToArray();
        }
        finally { buf.Dispose(); }

        // Wire: [sectionMask u8][container flag u8][opCount u16=1]
        //   [kind=StructFieldSet u8][slot u16][fieldIdx u8][i32 hp]
        // = 1 + 1 + 2 + 1 + 2 + 1 + 4 = 12 bytes.
        // A whole-mode kSet of the same struct would be:
        //   1 + 1 + 2 + 1 + 2 + 32 body = 39.
        Assert.Equal(12, bytes.Length);

        var reader = new SpanReader(bytes);
        dst.ApplyReplicatedDelta(ref reader);

        Assert.Equal(999, dst.Party[0].Hp);
        // Other fields unchanged through the field-set roundtrip.
        Assert.Equal(50, dst.Party[0].Mp);
        Assert.Equal(10, dst.Party[0].Atk);
    }

    [Fact]
    public void MultipleFieldMutations_EmitOnePerOp()
    {
        var src = new FieldStructTestEntity();
        var dst = new FieldStructTestEntity();
        src.Party.Add(new Atlas.Def.StatsBlock { Hp = 100, Mp = 50 });
        ApplyOneFrame(src, dst);

        // Three field setters → three kStructFieldSet ops on the wire.
        src.PartyAt(0).Hp = 200;
        src.PartyAt(0).Mp = 75;
        src.PartyAt(0).Atk = 30;

        ApplyOneFrame(src, dst);

        Assert.Equal(200, dst.Party[0].Hp);
        Assert.Equal(75, dst.Party[0].Mp);
        Assert.Equal(30, dst.Party[0].Atk);
    }

    [Fact]
    public void OuterAddPlusFieldMutation_DoNotInterfere()
    {
        var src = new FieldStructTestEntity();
        var dst = new FieldStructTestEntity();

        // Add slot 0 then immediately mutate one of its fields. Outer
        // Splice ships the full struct body for slot 0; the field-set
        // op records on the same tick should still apply on top.
        src.Party.Add(new Atlas.Def.StatsBlock { Hp = 10 });
        src.PartyAt(0).Hp = 20;

        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Party.Count);
        Assert.Equal(20, dst.Party[0].Hp);
    }

    [Fact]
    public void SameValueAssignment_DoesNotMarkDirty()
    {
        var src = new FieldStructTestEntity();
        src.Party.Add(new Atlas.Def.StatsBlock { Hp = 100 });
        FlushDirty(src);

        src.PartyAt(0).Hp = 100;  // same value

        var buf = FrameBufferSet.Rent();
        try
        {
            bool has = src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _);
            Assert.False(has);
        }
        finally { buf.Dispose(); }
    }

    private static void ApplyOneFrame(FieldStructTestEntity src, FieldStructTestEntity dst)
    {
        byte[] bytes;
        var buf = FrameBufferSet.Rent();
        try
        {
            if (!src.BuildAndConsumeReplicationFrame(
                    ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                    ref buf.OwnerDelta, ref buf.OtherDelta,
                    out _, out _)) return;
            bytes = buf.OwnerDelta.WrittenSpan.ToArray();
        }
        finally { buf.Dispose(); }

        var reader = new SpanReader(bytes);
        dst.ApplyReplicatedDelta(ref reader);
    }

    private static void FlushDirty(FieldStructTestEntity e)
    {
        var buf = FrameBufferSet.Rent();
        try
        {
            e.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _);
        }
        finally { buf.Dispose(); }
    }
}
