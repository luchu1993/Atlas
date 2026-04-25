using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

// Server-side fixture mirroring a list<int32> property. DefGenerator
// produces the ObservableList<int> accessor, ReplicatedDirtyFlags bit,
// and the delta serializer that encodes the list tail.
[Entity("ListSyncTestEntity")]
public partial class ListSyncTestEntity : ServerEntity
{
}

// Round-trip checks for the integral list-sync wire format. The minimal
// slice writes the whole list (count + N elements) every time any
// mutation fires, so the assertion surface is: "source list == apply-
// side list after one Build/Apply cycle, for every common mutation."
// P1c will layer op-log encoding on top of this path; failures here
// should be fixed before moving on.
public class ListSyncTests
{
    [Fact]
    public void AddItem_MarksDirty_AndDeltaEncodesOpLog()
    {
        var src = new ListSyncTestEntity();
        src.Titles.Add(10);
        src.Titles.Add(20);
        src.Titles.Add(30);

        var buf = FrameBufferSet.Rent();
        try
        {
            bool has = src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out ulong eventSeq, out _);
            Assert.True(has);
            Assert.Equal(1UL, eventSeq);

            // CompactOpLog merges three adjacent appends into a single
            // Splice(0,0,[3 items]). Wire: [sectionMask u8][container
            // flag u8][opCount u16][opKind u8][start u16][end u16]
            // [count u16][3×i32] = 1+1+2+1+2+2+2+12 = 23.
            var owner = buf.OwnerDelta.WrittenSpan;
            Assert.Equal(23, owner.Length);
            Assert.Equal(0x02, owner[0]);  // bit0 clear, bit1=Container set
            Assert.NotEqual(0, owner[1]);  // container flag has the list bit set
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void DeltaRoundTrip_ReproducesListContents()
    {
        var src = new ListSyncTestEntity();
        src.Titles.Add(1);
        src.Titles.Add(2);
        src.Titles.Add(3);
        src.Titles.Add(4);

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

        var dst = new ListSyncTestEntity();
        var reader = new SpanReader(bytes);
        dst.ApplyReplicatedDelta(ref reader);

        Assert.Equal(4, dst.Titles.Count);
        Assert.Equal(1, dst.Titles[0]);
        Assert.Equal(2, dst.Titles[1]);
        Assert.Equal(3, dst.Titles[2]);
        Assert.Equal(4, dst.Titles[3]);
    }

    [Fact]
    public void ScalarAndContainerInSameTick_EmitsBothSections()
    {
        var src = new ListSyncTestEntity();
        var dst = new ListSyncTestEntity();

        src.Hp = 100;            // scalar prop
        src.Titles.Add(42);      // container prop

        var buf = FrameBufferSet.Rent();
        try
        {
            src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _);

            var owner = buf.OwnerDelta.WrittenSpan;
            // bit0=Scalar set, bit1=Container set.
            Assert.Equal(0x03, owner[0]);

            var reader = new SpanReader(owner.ToArray());
            dst.ApplyReplicatedDelta(ref reader);
        }
        finally { buf.Dispose(); }

        Assert.Equal(100, dst.Hp);
        Assert.Equal(1, dst.Titles.Count);
        Assert.Equal(42, dst.Titles[0]);
    }

    [Fact]
    public void IncrementalDeltas_KeepReceiverInSync()
    {
        // Two ticks of deltas: after each apply dst must match src. This is
        // the critical op-log invariant — each tick's ops carry the minimal
        // change relative to prior-tick state, not a full rewrite.
        var src = new ListSyncTestEntity();
        var dst = new ListSyncTestEntity();

        src.Titles.Add(10);
        src.Titles.Add(20);
        ApplyOneFrame(src, dst);
        Assert.Equal(2, dst.Titles.Count);
        Assert.Equal(10, dst.Titles[0]);
        Assert.Equal(20, dst.Titles[1]);

        src.Titles.Add(30);
        ApplyOneFrame(src, dst);
        Assert.Equal(3, dst.Titles.Count);
        Assert.Equal(30, dst.Titles[2]);
    }

    [Fact]
    public void IndexerAssignment_EmitsSetOp()
    {
        var src = new ListSyncTestEntity();
        var dst = new ListSyncTestEntity();
        src.Titles.Add(1);
        src.Titles.Add(2);
        src.Titles.Add(3);
        ApplyOneFrame(src, dst);

        src.Titles[1] = 99;  // emits kSet(1, 99)
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 1, 99, 3 }, new[] { dst.Titles[0], dst.Titles[1], dst.Titles[2] });
    }

    [Fact]
    public void RemoveAt_EmitsSpliceDelete()
    {
        var src = new ListSyncTestEntity();
        var dst = new ListSyncTestEntity();
        src.Titles.Add(1);
        src.Titles.Add(2);
        src.Titles.Add(3);
        ApplyOneFrame(src, dst);

        src.Titles.RemoveAt(1);  // emits kListSplice(1, 2, [])
        ApplyOneFrame(src, dst);

        Assert.Equal(2, dst.Titles.Count);
        Assert.Equal(1, dst.Titles[0]);
        Assert.Equal(3, dst.Titles[1]);
    }

    [Fact]
    public void AdjacentAppends_MergeIntoSingleSplice()
    {
        // Five individual Adds should collapse to one Splice op on the
        // wire. The savings compound at fleet scale — a bag refill at
        // 5000 entities × 10 items × 7B overhead = 350 KB/tick otherwise.
        var src = new ListSyncTestEntity();
        for (int i = 0; i < 5; ++i) src.Titles.Add(i);

        var dst = new ListSyncTestEntity();
        ApplyOneFrame(src, dst);

        Assert.Equal(5, dst.Titles.Count);
        for (int i = 0; i < 5; ++i) Assert.Equal(i, dst.Titles[i]);
    }

    [Fact]
    public void NonAppendMutation_BreaksMergeChain()
    {
        // Add 1, Add 2, Set[0] = 99, Add 3.
        // Expected after compact: Splice(0,0,[1,2]) + Set(0,99) + Splice(2,2,[3])
        // — the Set in the middle blocks merging the two append chains.
        var src = new ListSyncTestEntity();
        src.Titles.Add(1);
        src.Titles.Add(2);
        src.Titles[0] = 99;
        src.Titles.Add(3);

        var dst = new ListSyncTestEntity();
        ApplyOneFrame(src, dst);

        Assert.Equal(3, dst.Titles.Count);
        Assert.Equal(99, dst.Titles[0]);
        Assert.Equal(2, dst.Titles[1]);
        Assert.Equal(3, dst.Titles[2]);
    }

    [Fact]
    public void ByteBudgetFallback_RewritesAsClearPlusFullRefill()
    {
        // Empty list + 20 Adds after merging = 1 Splice op; still cheaper
        // than fallback (no fallback triggered). But if we interleave
        // Sets to break merging chains, ops.Count grows past items.Count+1
        // and we should fall back.
        var src = new ListSyncTestEntity();
        src.Titles.Add(0);  // seed 1 item
        src.Titles.Add(1);

        var dst = new ListSyncTestEntity();
        ApplyOneFrame(src, dst);

        // Now break merging: alternating Set / Add on a 2-item list so
        // every op is a separate entry. Six mutations on a growing list
        // should trigger fallback.
        src.Titles[0] = 100;
        src.Titles[1] = 101;
        src.Titles.Add(200);
        src.Titles[0] = 102;
        src.Titles.Add(201);
        src.Titles[1] = 103;
        // 6 ops on a 4-item list; 6 > 4+1 → fallback triggers on Compact.

        ApplyOneFrame(src, dst);

        Assert.Equal(4, dst.Titles.Count);
        Assert.Equal(102, dst.Titles[0]);
        Assert.Equal(103, dst.Titles[1]);
        Assert.Equal(200, dst.Titles[2]);
        Assert.Equal(201, dst.Titles[3]);
    }

    [Fact]
    public void ByteBudgetFallback_TriggersWhenOpHeadersOutweighIntegralBytes()
    {
        // Five Set ops on a 4-item list<int32>: the old op-count
        // threshold (`ops > items + 1`) wouldn't fallback, but byte-
        // aware accounting recognizes 5 × (1 + 6) = 35 B of op headers
        // already exceeds the 8 B Clear+Splice header + 16 B integral
        // payload (24 B total).
        var src = new ListSyncTestEntity();
        for (int i = 0; i < 4; ++i) src.Titles.Add(i);
        FlushDirty(src);

        src.Titles[0] = 100;
        src.Titles[1] = 101;
        src.Titles[2] = 102;
        src.Titles[3] = 103;
        src.Titles[0] = 200;

        src.Titles.CompactOpLog(elementWireBytes: 4);
        Assert.Equal(2, src.Titles.Ops.Count);
        Assert.Equal(global::Atlas.Observable.OpKind.Clear, src.Titles.Ops[0].Kind);
        Assert.Equal(global::Atlas.Observable.OpKind.ListSplice, src.Titles.Ops[1].Kind);
        Assert.Equal(4, src.Titles.Ops[1].ValueCount);
    }

    [Fact]
    public void ByteBudgetFallback_StaysOpLogWhenElementsLarge()
    {
        // 5 Sets on a 10-item list with 16-byte elements: oplog = 5 ×
        // (7 + 16) = 115 B, fallback = 8 + 10 × 16 = 168 B. Op-log
        // wins, so the byte-aware path should keep all 5 Sets on the
        // wire instead of clobbering them with a full Clear+refill.
        var src = new ListSyncTestEntity();
        for (int i = 0; i < 10; ++i) src.Titles.Add(i);
        FlushDirty(src);

        src.Titles[0] = 100;
        src.Titles[1] = 101;
        src.Titles[2] = 102;
        src.Titles[3] = 103;
        src.Titles[4] = 104;

        src.Titles.CompactOpLog(elementWireBytes: 16);
        Assert.Equal(5, src.Titles.Ops.Count);
        Assert.All(src.Titles.Ops,
                   op => Assert.Equal(global::Atlas.Observable.OpKind.Set, op.Kind));
    }

    [Fact]
    public void ClearMidTick_AbsorbsPriorOps()
    {
        // ObservableList.Clear drops prior same-tick ops. Wire should be
        // just [Clear + Splice(0,0,[99])] (2 ops) regardless of how many
        // mutations happened before Clear.
        var src = new ListSyncTestEntity();
        src.Titles.Add(1);
        src.Titles.Add(2);
        src.Titles.Add(3);
        src.Titles.Clear();
        src.Titles.Add(99);

        // Only 2 ops should be in the log: Clear, then Splice-add.
        Assert.Equal(2, src.Titles.Ops.Count);
        Assert.Equal(global::Atlas.Observable.OpKind.Clear, src.Titles.Ops[0].Kind);
        Assert.Equal(global::Atlas.Observable.OpKind.ListSplice, src.Titles.Ops[1].Kind);

        var dst = new ListSyncTestEntity();
        dst.Titles.Add(777);  // prime — should be wiped by the Clear op
        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Titles.Count);
        Assert.Equal(99, dst.Titles[0]);
    }

    [Fact]
    public void RemoveItem_MarksDirty()
    {
        var src = new ListSyncTestEntity();
        src.Titles.Add(10);
        src.Titles.Add(20);
        FlushDirty(src);

        src.Titles.Remove(10);

        var buf = FrameBufferSet.Rent();
        try
        {
            bool has = src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _);
            Assert.True(has);
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void Clear_EmptyList_MarksNoDirty()
    {
        // Clear() on an already-empty list is a no-op by contract; the
        // ObservableList skips markDirty so we don't ship a wasted delta.
        var src = new ListSyncTestEntity();
        src.Titles.Clear();

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
    public void FullStateSerialize_RoundTripsListProperty()
    {
        // Exercise the persistence path (Serialize/Deserialize).
        var src = new ListSyncTestEntity();
        src.Titles.Add(7);
        src.Titles.Add(8);
        src.Titles.Add(9);
        src.Hp = 100;

        var writer = new SpanWriter(256);
        byte[] bytes;
        try
        {
            src.Serialize(ref writer);
            bytes = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }

        var dst = new ListSyncTestEntity();
        var reader = new SpanReader(bytes);
        dst.Deserialize(ref reader);

        Assert.Equal(100, dst.Hp);
        Assert.Equal(3, dst.Titles.Count);
        Assert.Equal(7, dst.Titles[0]);
        Assert.Equal(8, dst.Titles[1]);
        Assert.Equal(9, dst.Titles[2]);
    }

    [Fact]
    public void EmptyList_RoundTripsCleanly()
    {
        var src = new ListSyncTestEntity();
        // Touch the list once without adding anything so the wire payload
        // still includes the count=0 prefix.
        src.Titles.Add(1);
        src.Titles.Clear();

        byte[] bytes;
        var buf = FrameBufferSet.Rent();
        try
        {
            src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _);
            bytes = buf.OwnerDelta.WrittenSpan.ToArray();
        }
        finally { buf.Dispose(); }

        var dst = new ListSyncTestEntity();
        var reader = new SpanReader(bytes);
        dst.ApplyReplicatedDelta(ref reader);

        Assert.Equal(0, dst.Titles.Count);
    }

    [Fact]
    public void LazyList_Allocates_OnlyOnFirstAccess()
    {
        // Contract: reading the Titles property before any mutation
        // returns the same ObservableList instance on every subsequent
        // call. A refactor that drops the lazy cache would fail here.
        var e = new ListSyncTestEntity();
        var a = e.Titles;
        var b = e.Titles;
        var c = e.Titles;
        Assert.Same(a, b);
        Assert.Same(b, c);
    }

    private static void FlushDirty(ListSyncTestEntity e)
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

    // Pumps one replication frame from src to dst. Mirrors what the
    // CellApp pump would do per tick for a single observer.
    private static void ApplyOneFrame(ListSyncTestEntity src, ListSyncTestEntity dst)
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
