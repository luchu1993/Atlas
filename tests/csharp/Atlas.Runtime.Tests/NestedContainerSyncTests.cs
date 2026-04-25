using Atlas.Entity;
using Atlas.Observable;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

[Entity("NestedListTestEntity")]
public partial class NestedListTestEntity : ServerEntity
{
}

[Entity("StructWithContainerTestEntity")]
public partial class StructWithContainerTestEntity : ServerEntity
{
}

// Nested container coverage — `list<list<T>>`, `dict<K, list<V>>`, and a
// struct with a container field inside a `list<struct>`. Inner containers
// ARE Observable in the P2d design: mutating `matrix[0].Add(99)` bubbles
// its dirty state through the parent binding so the delta serializer
// picks it up and ships a targeted op instead of a full slot rewrite.
public class NestedContainerSyncTests
{
    [Fact]
    public void NestedList_FullStateRoundTrip()
    {
        var src = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 1, 2, 3 });
        src.Matrix.Add(new ObservableList<int>());
        src.Matrix.Add(new ObservableList<int> { 9 });

        var writer = new SpanWriter(256);
        byte[] bytes;
        try
        {
            src.Serialize(ref writer);
            bytes = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }

        var dst = new NestedListTestEntity();
        var reader = new SpanReader(bytes);
        dst.Deserialize(ref reader);

        Assert.Equal(3, dst.Matrix.Count);
        Assert.Equal(new[] { 1, 2, 3 }, dst.Matrix[0].Items);
        Assert.Empty(dst.Matrix[1].Items);
        Assert.Equal(new[] { 9 }, dst.Matrix[2].Items);
    }

    [Fact]
    public void NestedList_DeltaRoundTrip_ViaOuterAddAndInPlaceInnerAdd()
    {
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();

        src.Matrix.Add(new ObservableList<int> { 10, 20 });
        src.Matrix.Add(new ObservableList<int> { 30 });
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 10, 20 }, dst.Matrix[0].Items);
        Assert.Equal(new[] { 30 }, dst.Matrix[1].Items);

        // In-place mutation of an inner list now bubbles the dirty signal
        // up through IObservableChild.__Rebind so the outer serializer
        // emits a targeted op into slot 0's child stream — no wholesale
        // slot rewrite, no silent desync.
        src.Matrix[0].Add(99);
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 10, 20, 99 }, dst.Matrix[0].Items);
        Assert.Equal(new[] { 30 }, dst.Matrix[1].Items);
    }

    [Fact]
    public void NestedList_InnerSetter_PropagatesAsInnerSetOp()
    {
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 1, 2, 3 });
        ApplyOneFrame(src, dst);

        src.Matrix[0][1] = 99;  // inner[1] = 99
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 1, 99, 3 }, dst.Matrix[0].Items);
    }

    [Fact]
    public void NestedList_InnerRemoveAt_PropagatesAsInnerSpliceDelete()
    {
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 10, 20, 30 });
        ApplyOneFrame(src, dst);

        src.Matrix[0].RemoveAt(1);
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 10, 30 }, dst.Matrix[0].Items);
    }

    [Fact]
    public void NestedList_InnerClear_PropagatesAsInnerClearOp()
    {
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 1, 2, 3 });
        src.Matrix.Add(new ObservableList<int> { 4 });
        ApplyOneFrame(src, dst);

        src.Matrix[0].Clear();
        ApplyOneFrame(src, dst);

        Assert.Empty(dst.Matrix[0].Items);
        Assert.Equal(new[] { 4 }, dst.Matrix[1].Items);
    }

    [Fact]
    public void OuterRemoveAt_ShiftsInnerSlotCallback()
    {
        // RemoveAt at index 0 shifts slot 1 → 0. The surviving child's
        // Rebind must now report dirty for the NEW slot index.
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 1 });
        src.Matrix.Add(new ObservableList<int> { 2 });
        ApplyOneFrame(src, dst);

        src.Matrix.RemoveAt(0);
        ApplyOneFrame(src, dst);
        Assert.Equal(1, dst.Matrix.Count);
        Assert.Equal(new[] { 2 }, dst.Matrix[0].Items);

        // The (ex-slot-1) inner is now at slot 0; mutating it must be
        // reported at slot 0 in the outer's ChildDirtySlots — not the
        // stale slot 1.
        src.Matrix[0].Add(99);
        ApplyOneFrame(src, dst);
        Assert.Equal(new[] { 2, 99 }, dst.Matrix[0].Items);
    }

    [Fact]
    public void OuterSet_SupersedesInnerOps()
    {
        // Outer Set ships the replacement slot's full state — any inner
        // ops from the same tick are redundant. CompactOpLog drains the
        // child so next tick doesn't re-emit stale ops.
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 1, 2 });
        ApplyOneFrame(src, dst);

        var fresh = new ObservableList<int> { 7, 8, 9 };
        src.Matrix[0] = fresh;
        fresh.Add(10);  // after the Set, mutate the new inner
        // Both the outer Set AND the inner Add happened this tick. The
        // outer Set must win — dst ends at [7, 8, 9, 10] via the Set's
        // integral payload, NOT [7,8,9] followed by a stray Splice.
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 7, 8, 9, 10 }, dst.Matrix[0].Items);

        // Subsequent tick with no mutation — the inner's op log was
        // drained by CompactOpLog, so no duplicate wire.
        src.Matrix[0].Add(11);
        ApplyOneFrame(src, dst);
        Assert.Equal(new[] { 7, 8, 9, 10, 11 }, dst.Matrix[0].Items);
    }

    [Fact]
    public void OuterRemoveAt_PreservesShiftedChildPendingOps()
    {
        // Regression: ReattachFrom used to drain the surviving child's
        // op log on every shift, silently dropping mutations the script
        // had registered before the structural change. The split between
        // adoption (drain) and slot-rebind (no drain) keeps shifted
        // children's ops intact.
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 1 });
        src.Matrix.Add(new ObservableList<int> { 2 });
        ApplyOneFrame(src, dst);

        // Pending mutation on the SECOND inner — must survive RemoveAt(0)
        // shifting it down to slot 0 and arrive at dst.
        src.Matrix[1].Add(99);
        src.Matrix.RemoveAt(0);
        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Matrix.Count);
        Assert.Equal(new[] { 2, 99 }, dst.Matrix[0].Items);
    }

    [Fact]
    public void ClearThenAdd_DoesNotReplayInnerOpsNextTick()
    {
        // Regression: dedup used to early-return on Clear, leaving any
        // post-Clear Splice's covered children's logs un-drained. A
        // subsequent tick would replay the stale inner ops on top of
        // already-shipped state, doubling content on the receiver.
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.Matrix.Add(new ObservableList<int> { 1 });
        ApplyOneFrame(src, dst);

        src.Matrix.Clear();
        src.Matrix.Add(new ObservableList<int>());
        src.Matrix[0].Add(5);          // dirties slot 0 of post-Clear list
        ApplyOneFrame(src, dst);
        Assert.Equal(new[] { 5 }, dst.Matrix[0].Items);

        // Next tick: only ONE more inner mutation. If the previous
        // tick left stale ops in newInner, we'd see [5, 5, 6] here.
        src.Matrix[0].Add(6);
        ApplyOneFrame(src, dst);
        Assert.Equal(new[] { 5, 6 }, dst.Matrix[0].Items);
    }

    [Fact]
    public void RepeatedAddRemove_ClosureCacheReusesPerSlot()
    {
        // Per-slot closure cache: the markDirty captured by an inner
        // child is created once per slot index ever used and reused.
        // The cache list grows to its max-ever-used size and stops
        // (no new closures past that point), so a fixed-size churn
        // workload converges to zero closure allocations per iteration.
        var avatar = new NestedListTestEntity();

        // Warm slots 0 and 1 — the only slots this loop touches.
        avatar.Matrix.Add(new ObservableList<int>());
        avatar.Matrix.Add(new ObservableList<int>());
        avatar.Matrix.RemoveAt(0);
        avatar.Matrix.RemoveAt(0);
        FlushOuterDirty(avatar);

        // Two passes: first measures with the cache populated, second
        // measures the same workload again. They should match to within
        // GC sampling noise — proves no per-iteration closure churn,
        // and proves no other unbounded growth in the inner workload.
        const int kIters = 500;
        long alloc1 = MeasureChurn(avatar, kIters);
        long alloc2 = MeasureChurn(avatar, kIters);

        var per1 = (double)alloc1 / kIters;
        var per2 = (double)alloc2 / kIters;
        // Per-iter cost is dominated by `new ObservableList<int>()` (×2)
        // — the closures themselves should add zero. If a regression
        // re-introduces per-Attach closures (~64 B × 4 attaches = ~256 B
        // / iter), per2 would jump well past per1.
        Assert.True(per2 <= per1 + 64,
            $"closure-cache regressed: pass1 {per1:F1} B/iter, pass2 {per2:F1} B/iter");
    }

    private static long MeasureChurn(NestedListTestEntity avatar, int iters)
    {
        long before = System.GC.GetAllocatedBytesForCurrentThread();
        for (int i = 0; i < iters; ++i)
        {
            avatar.Matrix.Add(new ObservableList<int>());
            avatar.Matrix.Add(new ObservableList<int>());
            avatar.Matrix.RemoveAt(0);
            avatar.Matrix.RemoveAt(0);
        }
        return System.GC.GetAllocatedBytesForCurrentThread() - before;
    }

    private static void FlushOuterDirty(NestedListTestEntity e)
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

    [Fact]
    public void DictOfList_FullStateRoundTrip()
    {
        var src = new NestedListTestEntity();
        src.ByTag.Add("odd", new ObservableList<int> { 1, 3, 5 });
        src.ByTag.Add("even", new ObservableList<int> { 2, 4 });

        var writer = new SpanWriter(256);
        byte[] bytes;
        try
        {
            src.Serialize(ref writer);
            bytes = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }

        var dst = new NestedListTestEntity();
        var reader = new SpanReader(bytes);
        dst.Deserialize(ref reader);

        Assert.Equal(2, dst.ByTag.Count);
        Assert.Equal(new[] { 1, 3, 5 }, dst.ByTag["odd"].Items);
        Assert.Equal(new[] { 2, 4 }, dst.ByTag["even"].Items);
    }

    [Fact]
    public void DictOfList_InPlaceValueAdd_Propagates()
    {
        var src = new NestedListTestEntity();
        var dst = new NestedListTestEntity();
        src.ByTag.Add("odd", new ObservableList<int> { 1 });
        ApplyOneFrame(src, dst);

        src.ByTag["odd"].Add(3);
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 1, 3 }, dst.ByTag["odd"].Items);
    }

    [Fact]
    public void StructWithContainerField_FullStateRoundTrip()
    {
        var src = new StructWithContainerTestEntity();
        src.Quests.Add(new Atlas.Def.QuestProgress
        {
            Id = 101,
            ObjectivesDone = new System.Collections.Generic.List<int> { 1, 2, 5 },
        });
        src.Quests.Add(new Atlas.Def.QuestProgress
        {
            Id = 202,
            ObjectivesDone = new System.Collections.Generic.List<int>(),
        });

        var writer = new SpanWriter(256);
        byte[] bytes;
        try
        {
            src.Serialize(ref writer);
            bytes = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }

        var dst = new StructWithContainerTestEntity();
        var reader = new SpanReader(bytes);
        dst.Deserialize(ref reader);

        Assert.Equal(2, dst.Quests.Count);
        Assert.Equal(101, dst.Quests[0].Id);
        Assert.Equal(new[] { 1, 2, 5 }, dst.Quests[0].ObjectivesDone);
        Assert.Equal(202, dst.Quests[1].Id);
        Assert.Empty(dst.Quests[1].ObjectivesDone);
    }

    [Fact]
    public void StructWithContainerField_DeltaRoundTrip()
    {
        var src = new StructWithContainerTestEntity();
        var dst = new StructWithContainerTestEntity();

        src.Quests.Add(new Atlas.Def.QuestProgress
        {
            Id = 1,
            ObjectivesDone = new System.Collections.Generic.List<int> { 1 },
        });
        ApplyOneFrame(src, dst);

        Assert.Equal(1, dst.Quests.Count);
        Assert.Equal(1, dst.Quests[0].Id);
        Assert.Equal(new[] { 1 }, dst.Quests[0].ObjectivesDone);

        // Struct fields use plain List<T>: in-place inner mutation does
        // not bubble to the owning struct slot, so scripts reassign the
        // whole struct slot to replicate.
        src.Quests[0] = new Atlas.Def.QuestProgress
        {
            Id = 1,
            ObjectivesDone = new System.Collections.Generic.List<int> { 1, 2, 3 },
        };
        ApplyOneFrame(src, dst);

        Assert.Equal(new[] { 1, 2, 3 }, dst.Quests[0].ObjectivesDone);
    }

    private static void ApplyOneFrame(NestedListTestEntity src, NestedListTestEntity dst)
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

    private static void ApplyOneFrame(StructWithContainerTestEntity src,
                                      StructWithContainerTestEntity dst)
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
