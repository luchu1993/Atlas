using Atlas.Components;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

// End-to-end wire-format check for the Component delta path:
//   AvatarWithAbility (components-only entity)
//   AvatarAbility (cooldown=own_client, ranking=own_client)
//
// The entity's own SerializeOwnerDelta writes a sectionMask byte; bit 2
// signals "component section follows". Per-component delta mirrors the
// entity-level wire layout: [u8 sectionMask][u64 scalarFlags][values].
//
// AddComponent flips _dirtyComponents but no property is yet dirty, so
// the audience predicates return false and BuildAndConsume bails out
// (no phantom frame). _dirtyComponents stays set as harmless internal
// state until the first real property mutation triggers a real frame.
public class ComponentReplicationFrameTests
{
    [Fact]
    public void NoPropertyChange_NoFrameEmitted()
    {
        var avatar = new AvatarWithAbility();
        avatar.AddComponent<AvatarAbility>();
        var buf = FrameBufferSet.Rent();
        try
        {
            bool any = avatar.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out var eventSeq, out var volatileSeq);

            Assert.False(any);
            Assert.Equal(0UL, eventSeq);
            Assert.Equal(0UL, volatileSeq);
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void OwnerVisibleProp_AppearsInOwnerDelta()
    {
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        var buf = FrameBufferSet.Rent();
        try
        {
            ability.Cooldown = 1.5f;
            bool any = avatar.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta,
                out var eventSeq, out _);
            Assert.True(any);
            Assert.Equal(1UL, eventSeq);

            // Owner delta wire shape:
            //   [u8 sectionMask=0x04 — components only]
            //   [PackedInt count=1]
            //   [u8 slot_idx=1]
            //   [u8 component_sectionMask=0x01 — scalar only]
            //   [u64 scalarFlags=0x01 — propIdx 0 (cooldown)]
            //   [float 1.5]
            var reader = new SpanReader(buf.OwnerDelta.WrittenSpan);
            Assert.Equal(0x04, reader.ReadByte());
            Assert.Equal(1u, reader.ReadPackedUInt32());
            Assert.Equal(1, reader.ReadByte());           // slot_idx
            Assert.Equal(0x01, reader.ReadByte());        // component sectionMask
            Assert.Equal(1UL, reader.ReadUInt64());       // scalarFlags
            Assert.Equal(1.5f, reader.ReadFloat());
            Assert.Equal(0, reader.Remaining);

            // Other delta: cooldown is own_client only, so bit 2 is NOT set.
            var otherReader = new SpanReader(buf.OtherDelta.WrittenSpan);
            Assert.Equal(0x00, otherReader.ReadByte());
            Assert.Equal(0, otherReader.Remaining);
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void InheritedAndOwnProps_BothFlow()
    {
        // Cooldown lives on AbilityComponent (base), Ranking on AvatarAbility
        // (derived). Hierarchy-flat propIdx means cooldown=0, ranking=1.
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        var buf = FrameBufferSet.Rent();
        try
        {
            ability.Cooldown = 2.0f;
            ability.Ranking  = 7;
            avatar.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta, out _, out _);

            var reader = new SpanReader(buf.OwnerDelta.WrittenSpan);
            Assert.Equal(0x04, reader.ReadByte());
            Assert.Equal(1u, reader.ReadPackedUInt32());
            Assert.Equal(1, reader.ReadByte());
            Assert.Equal(0x01, reader.ReadByte());
            Assert.Equal(0b11UL, reader.ReadUInt64());    // bits 0+1
            Assert.Equal(2.0f, reader.ReadFloat());       // cooldown (propIdx 0)
            Assert.Equal(7, reader.ReadInt32());          // ranking (propIdx 1)
        }
        finally { buf.Dispose(); }
    }

    [Fact]
    public void DirtyClearedAfterFrame()
    {
        // ClearDirtyComponents must reset both the entity bitmap AND each
        // component's _dirtyFlags so the next pump returns false.
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        var buf = FrameBufferSet.Rent();
        try
        {
            ability.Cooldown = 0.25f;
            avatar.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta, out _, out _);
            Assert.False(ability.IsDirty);

            buf.ResetAll();
            bool any = avatar.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta, out _, out _);
            Assert.False(any);
        }
        finally { buf.Dispose(); }
    }
}

// MonsterAbility has aggro=all_clients — exercises the OTHER audience
// path that AvatarAbility (own_client only) doesn't reach.
public class MonsterAbilityFrameTests
{
    private static void AssertAggroDelta(System.ReadOnlySpan<byte> span)
    {
        var r = new SpanReader(span);
        Assert.Equal(0x04, r.ReadByte());
        Assert.Equal(1u, r.ReadPackedUInt32());
        Assert.Equal(1, r.ReadByte());            // slot_idx
        Assert.Equal(0x01, r.ReadByte());         // scalar only
        Assert.Equal(0b10UL, r.ReadUInt64());     // bit 1 = aggro
        Assert.Equal(1500, r.ReadInt32());
    }

    [Fact]
    public void AllClientsProp_AppearsInBothAudiences()
    {
        var monster = new MonsterWithAbility();
        var ability = monster.AddComponent<MonsterAbility>();
        var buf = FrameBufferSet.Rent();
        try
        {
            ability.Aggro = 1500;
            monster.BuildAndConsumeReplicationFrame(
                ref buf.OwnerSnapshot, ref buf.OtherSnapshot,
                ref buf.OwnerDelta, ref buf.OtherDelta, out _, out _);

            // Aggro is all_clients → both owner and other deltas carry it.
            // MonsterAbility has propIdx 0 = cooldown (inherited, own_client),
            // propIdx 1 = aggro (all_clients). Only aggro is dirty.
            AssertAggroDelta(buf.OwnerDelta.WrittenSpan);
            AssertAggroDelta(buf.OtherDelta.WrittenSpan);
        }
        finally { buf.Dispose(); }
    }
}
