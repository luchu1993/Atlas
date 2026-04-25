using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

// Locks in the receive-side container-reuse contract for replicated
// list / dict properties. The generator emits ApplyReplicatedDelta paths
// that call ClearWithoutDirty + AddWithoutDirty on the *existing*
// ObservableList / ObservableDict instance instead of allocating a new
// one — see PropertyCodec.EmitListRead / EmitDictRead.
//
// Why this matters: under stress (5000 entities × 3 components × 2
// container props × 10 Hz), any reshape that swaps the instance on every
// delta would drive ~150k allocations/s on the client. Existing
// ListSyncTests / DictSyncTests assert *content* equivalence after
// apply; they pass equally well whether the receiver reuses or
// reallocates. This file asserts *reference identity* so a regression
// (e.g. someone replaces `__list.ClearWithoutDirty()` with
// `__list = new ObservableList<int>(...)`) fails fast.
//
// Whole-replace via fallback is exercised because the generator path
// for "first non-empty receive" and "fallback Clear+Splice" land in the
// same EmitListRead-emitted code: get the existing instance, clear,
// repopulate. Op-log path (per-element ops) reuses the instance by
// construction; covered separately by ListSyncTests but re-verified
// here for symmetry.
public class ContainerReuseTests
{
    [Fact]
    public void List_OpLogDelta_ReusesContainerInstance()
    {
        var src = new ListSyncTestEntity();
        var dst = new ListSyncTestEntity();

        // First tick: lazy-allocate the destination's ObservableList by
        // touching it before any delta arrives. This pins the reference
        // we want to verify is preserved across applies.
        var initialDstList = dst.Titles;

        src.Titles.Add(1);
        src.Titles.Add(2);
        ApplyOneFrame(src, dst);
        Assert.Same(initialDstList, dst.Titles);

        // Second tick: incremental op-log path (one Add).
        src.Titles.Add(3);
        ApplyOneFrame(src, dst);
        Assert.Same(initialDstList, dst.Titles);
        Assert.Equal(3, dst.Titles.Count);
    }

    [Fact]
    public void List_FallbackWholeReplace_ReusesContainerInstance()
    {
        // Trigger the byte-aware op-log → Clear+Splice fallback. Even on
        // the whole-replace path, the receive-side container instance
        // must stay the same.
        var src = new ListSyncTestEntity();
        var dst = new ListSyncTestEntity();

        for (int i = 0; i < 4; ++i) src.Titles.Add(i);
        ApplyOneFrame(src, dst);
        var initialDstList = dst.Titles;
        Assert.Equal(4, dst.Titles.Count);

        // Five Sets on a 4-item list with 4-byte elements: oplog headers
        // (5 × 7 = 35 B) outweigh the 8 B Clear+Splice header + 16 B
        // integral payload — fallback fires (mirrors the threshold case
        // in ListSyncTests.ByteBudgetFallback_TriggersWhenOpHeadersOutweighIntegralBytes).
        src.Titles[0] = 100;
        src.Titles[1] = 101;
        src.Titles[2] = 102;
        src.Titles[3] = 103;
        src.Titles[0] = 200;
        ApplyOneFrame(src, dst);

        Assert.Same(initialDstList, dst.Titles);
        Assert.Equal(4, dst.Titles.Count);
        Assert.Equal(200, dst.Titles[0]);
        Assert.Equal(101, dst.Titles[1]);
        Assert.Equal(102, dst.Titles[2]);
        Assert.Equal(103, dst.Titles[3]);
    }

    [Fact]
    public void Dict_OpLogDelta_ReusesContainerInstance()
    {
        var src = new DictSyncTestEntity();
        var dst = new DictSyncTestEntity();

        var initialDstDict = dst.Scores;

        src.Scores.Add("a", 1);
        src.Scores.Add("b", 2);
        ApplyOneFrame(src, dst);
        Assert.Same(initialDstDict, dst.Scores);

        src.Scores["a"] = 99;
        ApplyOneFrame(src, dst);
        Assert.Same(initialDstDict, dst.Scores);
        Assert.Equal(99, dst.Scores["a"]);
    }

    [Fact]
    public void Dict_FallbackWholeReplace_ReusesContainerInstance()
    {
        var src = new DictSyncTestEntity();
        var dst = new DictSyncTestEntity();

        for (int i = 0; i < 4; ++i) src.Scores.Add($"k{i}", i);
        ApplyOneFrame(src, dst);
        var initialDstDict = dst.Scores;
        Assert.Equal(4, dst.Scores.Count);

        // Force fallback with an op pattern whose byte cost beats the
        // integral fallback: alternating Sets keep ops ungrouped, and a
        // string-keyed dict has fat per-op headers.
        src.Scores["k0"] = 100;
        src.Scores["k1"] = 101;
        src.Scores["k2"] = 102;
        src.Scores["k3"] = 103;
        src.Scores["k0"] = 200;
        src.Scores["k1"] = 201;
        ApplyOneFrame(src, dst);

        Assert.Same(initialDstDict, dst.Scores);
        Assert.Equal(4, dst.Scores.Count);
        Assert.Equal(200, dst.Scores["k0"]);
        Assert.Equal(201, dst.Scores["k1"]);
        Assert.Equal(102, dst.Scores["k2"]);
        Assert.Equal(103, dst.Scores["k3"]);
    }

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
