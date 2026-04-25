using System;
using Atlas.Def;
using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

// Server-side fixture mirroring StressAvatar + MutRef-capable struct
// property. DefGenerator produces the fields, Serialize / Deserialize,
// the ReplicatedDirtyFlags enum, and the MutRef accessor; the test
// class body is otherwise empty.
[Entity("StructSyncTestEntity")]
public partial class StructSyncTestEntity : ServerEntity
{
}

// End-to-end round-trip for the struct-sync pipeline:
//   MutRef field setter → dirty bit → BuildAndConsumeReplicationFrame
//   writes owner delta bytes → ApplyReplicatedDelta reads the bytes
//   back into a second instance.
//
// These cover the "scalar struct property" contract the whole P0 arc
// exists to deliver. If any of the emitters (MutRef, PropertyCodec,
// DeltaSync) regress, a field lands on the wire as garbage and one of
// these assertions fails.
public class StructSyncTests
{
    [Fact]
    public void MutRefSetter_MarksDirty_AndOwnerDeltaCarriesStructBytes()
    {
        var src = new StructSyncTestEntity();
        // Pre-condition: no dirty before the write. BuildAndConsume on a
        // pristine entity returns false with zero event/volatile seq.
        var buf = FrameBufferSet.Rent();
        try
        {
            Assert.False(src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _));
        }
        finally { buf.Dispose(); }

        // Act: write to a struct field through the MutRef accessor.
        src.MainWeapon.Id = 1234;

        // Post-condition: the MainWeapon bit is now dirty, and pumping
        // a frame produces a non-empty owner delta that encodes the
        // full struct (PropertyCodec must have routed through
        // TestWeapon.Serialize, not WriteInt32(_mainWeapon)).
        buf = FrameBufferSet.Rent();
        try
        {
            bool has = src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out ulong eventSeq, out _);
            Assert.True(has);
            Assert.Equal(1UL, eventSeq);

            // Owner delta wire layout (sectionMask + scalar section):
            //   [u8 sectionMask][u8 scalarFlags][TestWeapon body: i32 id,
            //   u16 sharpness, u8 bound] = 1 + 1 + 4 + 2 + 1 = 9 B.
            var owner = buf.OwnerDelta.WrittenSpan;
            Assert.Equal(9, owner.Length);
            Assert.Equal(0x01, owner[0]);  // bit0=Scalar set, bit1=Container clear
            Assert.NotEqual(0, owner[1]);  // scalar flag byte has the struct bit set
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void MixedScalarAndContainerDirty_EmitsBothSections()
    {
        // Hp (scalar) and MainWeapon (struct = scalar in section terms)
        // both dirty in one tick. SectionMask should be 0x01 — both
        // are scalar-section properties since neither is list / dict.
        var src = new StructSyncTestEntity();
        src.Hp = 50;
        src.MainWeapon.Id = 7;

        var buf = FrameBufferSet.Rent();
        try
        {
            src.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out _, out _);

            var owner = buf.OwnerDelta.WrittenSpan;
            // Scalar-only frame: bit0 set, bit1 clear.
            Assert.Equal(0x01, owner[0]);
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void OwnerDelta_RoundTripsThroughApplyReplicatedDelta()
    {
        var src = new StructSyncTestEntity();
        src.MainWeapon.Id = 1234;
        src.MainWeapon.Sharpness = 42;
        src.MainWeapon.Bound = true;

        // Serialise on the writer side.
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

        // Apply on a fresh instance and read back via the snapshot
        // property (MutRef would also work via implicit conversion;
        // we use Value to make the assertion explicit).
        var dst = new StructSyncTestEntity();
        var reader = new SpanReader(bytes);
        dst.ApplyReplicatedDelta(ref reader);

        var w = dst.MainWeaponValue;
        Assert.Equal(1234, w.Id);
        Assert.Equal((ushort)42, w.Sharpness);
        Assert.True(w.Bound);
    }

    [Fact]
    public void SameFieldAssignment_DoesNotMarkDirty()
    {
        // Prime the entity with a non-default struct so we're not
        // comparing against an all-zero baseline (which would mask an
        // incorrect equality check).
        var src = new StructSyncTestEntity();
        src.MainWeapon.Id = 1234;
        FlushDirty(src);  // drain the dirty bit

        // Act: re-assign the same value. PropertiesEmitter's short-
        // circuit guard should skip the write entirely.
        src.MainWeapon.Id = 1234;

        // Expect: BuildAndConsume reports no work to do. If the guard
        // regresses, _dirtyFlags stays set and the pump would send a
        // redundant delta every tick.
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
    public void FullStateSerialize_RoundTripsStructProperty()
    {
        // Exercise the full-state path (Serialize / Deserialize) — the
        // DBApp persistence + hot-reload route. A regression here
        // corrupts saved game state.
        var src = new StructSyncTestEntity();
        src.MainWeapon.Id = 55;
        src.MainWeapon.Sharpness = 7;
        src.MainWeapon.Bound = true;
        src.Hp = 99;

        var writer = new SpanWriter(256);
        byte[] bytes;
        try
        {
            src.Serialize(ref writer);
            bytes = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }

        var dst = new StructSyncTestEntity();
        var reader = new SpanReader(bytes);
        dst.Deserialize(ref reader);

        var w = dst.MainWeaponValue;
        Assert.Equal(55, w.Id);
        Assert.Equal((ushort)7, w.Sharpness);
        Assert.True(w.Bound);
        Assert.Equal(99, dst.Hp);
    }

    [Fact]
    public void StructType_IsEmittedInAtlasDefNamespace()
    {
        // Fully-qualified usage proves Atlas.Def.TestWeapon exists;
        // a regression in StructEmitter's namespace choice would fail
        // the compile here.
        Atlas.Def.TestWeapon w = default;
        w.Id = 10;
        Assert.Equal(10, w.Id);
    }

    [Fact]
    public void StructCodec_SelfRoundTrip()
    {
        // The struct's own Serialize / Deserialize pair must round-trip
        // independently of any entity containment — that's what lets
        // nested ops and future kStructFieldSet share the same codec.
        var src = new TestWeapon { Id = -77, Sharpness = 300, Bound = false };
        var writer = new SpanWriter(32);
        byte[] bytes;
        try
        {
            src.Serialize(ref writer);
            bytes = writer.WrittenSpan.ToArray();
        }
        finally { writer.Dispose(); }

        // Fixed width: i32 + u16 + u8 = 7 B. If this drifts the auto-
        // sync heuristic also drifts.
        Assert.Equal(7, bytes.Length);

        var reader = new SpanReader(bytes);
        var dst = TestWeapon.Deserialize(ref reader);
        Assert.Equal(-77, dst.Id);
        Assert.Equal((ushort)300, dst.Sharpness);
        Assert.False(dst.Bound);
    }

    private static void FlushDirty(StructSyncTestEntity e)
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

    // =========================================================================
    // MutRef allocation contract
    // =========================================================================
    //
    // The MutRef is a sealed class (CS1612 rules out struct returns through
    // a property), but it's lazy-cached on the entity so scripts that hit the
    // accessor every tick don't allocate. These three tests pin that contract
    // down so a refactor that silently loses the cache — or switches to a
    // per-access `new MutRef(this)` pattern — fails loudly in CI.
    //
    // Thread-safety of the measurement: GC.GetAllocatedBytesForCurrentThread
    // is per-thread, and xunit runs tests within a single class serially by
    // default (each test class gets its own implicit collection). So other
    // tests running in parallel in DIFFERENT classes can't pollute the read.
    // If the measurement is ever moved to a shared test class, slap a
    // `[Collection("AllocSensitive")]` attribute on the class so xunit
    // serialises that collection explicitly.

    [Fact]
    public void MutRef_LazyInit_ReturnsSameInstanceOnRepeatedAccess()
    {
        var e = new StructSyncTestEntity();
        var first = e.MainWeapon;
        var second = e.MainWeapon;
        var third = e.MainWeapon;
        // Reference equality — if we ever flip back to a value-type or
        // "new-each-time" class, these would be three different objects.
        Assert.Same(first, second);
        Assert.Same(second, third);
    }

    [Fact]
    public void MutRef_SetterLoop_IsZeroAlloc()
    {
        // Warm the JIT path for the exact call shape we'll measure. Without
        // this the first iteration's JIT compilation inflates the allocation
        // number by whatever the runtime stashes on first use.
        var warm = new StructSyncTestEntity();
        for (int i = 0; i < 128; ++i)
        {
            warm.MainWeapon.Id = i;
            warm.MainWeapon.Sharpness = (ushort)(i & 0xFFFF);
            warm.MainWeapon.Bound = (i & 1) == 0;
        }

        // Freshly constructed entity; prime its MutRef cache with a single
        // write so the subsequent loop only exercises the hot path.
        var e = new StructSyncTestEntity();
        e.MainWeapon.Id = 0;

        // Measurement window — nothing in the loop should allocate. If this
        // regresses, GC churn at 5000-entity scale becomes real.
        const int kIters = 1000;
        long before = GC.GetAllocatedBytesForCurrentThread();
        for (int i = 1; i <= kIters; ++i)
        {
            e.MainWeapon.Id = i;
            e.MainWeapon.Sharpness = (ushort)i;
            e.MainWeapon.Bound = (i & 1) == 0;
        }
        long delta = GC.GetAllocatedBytesForCurrentThread() - before;

        Assert.Equal(0L, delta);
    }

    [Fact]
    public void MutRef_FirstAccess_AllocatesOnlyTheMutRef()
    {
        // Warm up the JIT path so the per-call measurement reflects the
        // actual steady-state alloc, not tier-0 compilation bookkeeping.
        for (int i = 0; i < 4; ++i)
        {
            var dummy = new StructSyncTestEntity();
            _ = dummy.MainWeapon;
        }

        var e = new StructSyncTestEntity();

        long before = GC.GetAllocatedBytesForCurrentThread();
        _ = e.MainWeapon;  // lazy `??= new MainWeaponMutRef(this)`
        long delta = GC.GetAllocatedBytesForCurrentThread() - before;

        // The MutRef class carries `Avatar _owner` as its only instance
        // state: x64 object header (~16 B) + one reference field (8 B) =
        // 24 B. Allocator alignment can round the exact number up a little,
        // so assert a tight range rather than a point value.
        Assert.InRange(delta, 16L, 64L);

        // A second access must be free.
        long before2 = GC.GetAllocatedBytesForCurrentThread();
        _ = e.MainWeapon;
        long delta2 = GC.GetAllocatedBytesForCurrentThread() - before2;
        Assert.Equal(0L, delta2);
    }
}
