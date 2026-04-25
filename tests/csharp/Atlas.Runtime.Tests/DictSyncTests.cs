using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

[Entity("DictSyncTestEntity")]
public partial class DictSyncTestEntity : ServerEntity
{
}

// Dict analog of ListSyncTests. Same [u16 count][K v]* wire contract,
// same lazy-ObservableDict + markDirty plumbing.
public class DictSyncTests
{
    [Fact]
    public void AddKv_MarksDirty_AndDeltaEncodesOpLog()
    {
        var src = new DictSyncTestEntity();
        src.Scores.Add("alice", 10);
        src.Scores.Add("bob", 20);

        var buf = FrameBufferSet.Rent();
        try
        {
            bool has = src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out ulong eventSeq, out _);
            Assert.True(has);
            Assert.Equal(1UL, eventSeq);

            // Op-log wire per DictSet: [opKind u8][string k][i32 v].
            //   "alice" (1+5 packed-len) + 4 + kind 1 = 11
            //   "bob"   (1+3 packed-len) + 4 + kind 1 = 9
            // Frame: [sectionMask u8][container flag u8][opCount u16] 1
            // + 1 + 2 + 11 + 9 = 24.
            var owner = buf.OwnerDelta.WrittenSpan;
            Assert.Equal(24, owner.Length);
            Assert.Equal(0x02, owner[0]);
            Assert.NotEqual(0, owner[1]);
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void DeltaRoundTrip_ReproducesDictContents()
    {
        var src = new DictSyncTestEntity();
        src.Scores.Add("a", 1);
        src.Scores.Add("b", 2);
        src.Scores.Add("c", 3);

        byte[] bytes;
        var buf = FrameBufferSet.Rent();
        try
        {
            Assert.True(src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _));
            bytes = buf.OwnerDelta.WrittenSpan.ToArray();
        }
        finally { buf.Dispose(); }

        var dst = new DictSyncTestEntity();
        var reader = new SpanReader(bytes);
        dst.ApplyReplicatedDelta(ref reader);

        Assert.Equal(3, dst.Scores.Count);
        Assert.Equal(1, dst.Scores["a"]);
        Assert.Equal(2, dst.Scores["b"]);
        Assert.Equal(3, dst.Scores["c"]);
    }

    [Fact]
    public void IncrementalDeltas_KeepReceiverInSync()
    {
        var src = new DictSyncTestEntity();
        var dst = new DictSyncTestEntity();

        src.Scores.Add("a", 1);
        src.Scores.Add("b", 2);
        ApplyOneFrame(src, dst);
        Assert.Equal(2, dst.Scores.Count);
        Assert.Equal(1, dst.Scores["a"]);

        src.Scores["a"] = 100;  // overwrite → DictSet op
        src.Scores.Add("c", 3);
        ApplyOneFrame(src, dst);
        Assert.Equal(3, dst.Scores.Count);
        Assert.Equal(100, dst.Scores["a"]);
        Assert.Equal(3, dst.Scores["c"]);
    }

    [Fact]
    public void Remove_EmitsEraseOp()
    {
        var src = new DictSyncTestEntity();
        var dst = new DictSyncTestEntity();
        src.Scores.Add("x", 1);
        src.Scores.Add("y", 2);
        ApplyOneFrame(src, dst);

        Assert.True(src.Scores.Remove("x"));  // kDictErase
        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Scores.Count);
        Assert.False(dst.Scores.ContainsKey("x"));
        Assert.Equal(2, dst.Scores["y"]);
    }

    [Fact]
    public void SameKeyMultipleSets_DedupToLastValue()
    {
        // Updating the same key N times should land exactly one op on
        // the wire with the final value.
        var src = new DictSyncTestEntity();
        src.Scores.Add("k", 1);
        src.Scores["k"] = 2;
        src.Scores["k"] = 3;
        src.Scores["k"] = 4;

        var dst = new DictSyncTestEntity();
        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Scores.Count);
        Assert.Equal(4, dst.Scores["k"]);
    }

    [Fact]
    public void SetThenErase_SameKey_BothSurvive()
    {
        // Today's dedup keeps the LAST op per key; Set followed by Erase
        // on the same key means only the Erase goes on the wire. Remote
        // receiver that never saw the Set is still correct: the key
        // doesn't exist afterwards.
        var src = new DictSyncTestEntity();
        src.Scores.Add("a", 1);  // seed
        var dst = new DictSyncTestEntity();
        ApplyOneFrame(src, dst);

        src.Scores.Add("b", 2);   // DictSet
        src.Scores.Remove("b");   // DictErase → shadows prior Set
        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Scores.Count);
        Assert.False(dst.Scores.ContainsKey("b"));
    }

    [Fact]
    public void ByteBudgetFallback_ReplacesExcessOpsWithClearPlusRefill()
    {
        // Seed dst so the fallback branch has something to clear out.
        var src = new DictSyncTestEntity();
        var dst = new DictSyncTestEntity();
        src.Scores.Add("x", 1);
        src.Scores.Add("y", 2);
        ApplyOneFrame(src, dst);

        // Churn: 5 ops on distinct keys, but list only grows to 4
        // entries. Dedup doesn't collapse distinct keys, so the budget
        // check kicks in.
        src.Scores.Remove("x");
        src.Scores.Add("a", 10);
        src.Scores.Add("b", 20);
        src.Scores.Add("c", 30);
        src.Scores.Remove("y");
        // After: dict has {a, b, c}. 5 ops on 3 items → fallback.

        ApplyOneFrame(src, dst);

        Assert.Equal(3, dst.Scores.Count);
        Assert.Equal(10, dst.Scores["a"]);
        Assert.Equal(20, dst.Scores["b"]);
        Assert.Equal(30, dst.Scores["c"]);
        Assert.False(dst.Scores.ContainsKey("x"));
        Assert.False(dst.Scores.ContainsKey("y"));
    }

    [Fact]
    public void ClearMidTick_AbsorbsPriorOps()
    {
        var src = new DictSyncTestEntity();
        src.Scores.Add("a", 1);
        src.Scores.Add("b", 2);
        src.Scores.Clear();
        src.Scores.Add("c", 3);

        Assert.Equal(2, src.Scores.Ops.Count);
        Assert.Equal(global::Atlas.Observable.OpKind.Clear, src.Scores.Ops[0].Kind);
        Assert.Equal(global::Atlas.Observable.OpKind.DictSet, src.Scores.Ops[1].Kind);

        var dst = new DictSyncTestEntity();
        dst.Scores.Add("stale", 999);
        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Scores.Count);
        Assert.Equal(3, dst.Scores["c"]);
    }

    [Fact]
    public void IndexerAssignmentToSameValue_DoesNotMarkDirty()
    {
        var src = new DictSyncTestEntity();
        src.Scores.Add("k", 5);
        FlushDirty(src);

        src.Scores["k"] = 5;  // same value — ObservableDict short-circuits

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

    [Fact]
    public void Clear_EmptyDict_MarksNoDirty()
    {
        var src = new DictSyncTestEntity();
        src.Scores.Clear();

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

    [Fact]
    public void FullStateSerialize_RoundTripsDictProperty()
    {
        var src = new DictSyncTestEntity();
        src.Scores.Add("x", 42);
        src.Scores.Add("y", -7);
        src.Hp = 50;

        var writer = new SpanWriter(256);
        byte[] bytes;
        try
        {
            src.Serialize(ref writer);
            bytes = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }

        var dst = new DictSyncTestEntity();
        var reader = new SpanReader(bytes);
        dst.Deserialize(ref reader);

        Assert.Equal(50, dst.Hp);
        Assert.Equal(2, dst.Scores.Count);
        Assert.Equal(42, dst.Scores["x"]);
        Assert.Equal(-7, dst.Scores["y"]);
    }

    [Fact]
    public void LazyDict_Allocates_OnlyOnFirstAccess()
    {
        var e = new DictSyncTestEntity();
        var a = e.Scores;
        var b = e.Scores;
        var c = e.Scores;
        Assert.Same(a, b);
        Assert.Same(b, c);
    }

    private static void FlushDirty(DictSyncTestEntity e)
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

    private static void ApplyOneFrame(DictSyncTestEntity src, DictSyncTestEntity dst)
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
}
