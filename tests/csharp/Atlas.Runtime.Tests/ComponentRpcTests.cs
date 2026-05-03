using System.Collections.Generic;
using Atlas.Components;
using Atlas.Def;
using Atlas.Rpc;
using Atlas.Serialization;
using Xunit;

// User partial lives in Atlas.Components — the namespace the generator
// emits the class into. CS0759 fires if it's anywhere else, even if the
// file imports the namespace via `using`.
namespace Atlas.Components
{
    public sealed partial class AvatarAbility
    {
        public uint LastEchoSeq;
        public long LastEchoTs;
        public int EchoCallCount;
        public List<int>? LastTags;
        public Dictionary<string, int>? LastKv;
        public Atlas.Def.TestWeapon LastWeapon;

        public partial void Echo(uint seq, long ts)
        {
            LastEchoSeq = seq;
            LastEchoTs = ts;
            EchoCallCount++;
        }

        public partial void ApplyTags(System.Collections.Generic.List<int> tags) { LastTags = tags; }
        public partial void ApplyKv(System.Collections.Generic.Dictionary<string, int> kv) { LastKv = kv; }
        public partial void EquipTestWeapon(Atlas.Def.TestWeapon w) { LastWeapon = w; }
    }
}

namespace Atlas.Tests
{

// Verifies the component RPC pipeline:
//   entity.def declares Echo on AvatarAbility's <cell_methods>;
//   ComponentEmitter emits the partial method declaration + send stub;
//   DispatcherEmitter routes inbound rpc_id (slot=1, methodIdx=1) into
//   the AvatarAbility instance attached to the entity.
public class ComponentRpcTests
{
    [Fact]
    public void DispatchBaseRpc_RoutesToComponent()
    {
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();

        // Methods sort alphabetically: ApplyKv(1), ApplyTags(2), Echo(3),
        // EquipTestWeapon(4). slot=1 is AvatarAbility, direction=Base(0x03).
        ushort entityTypeId = avatar.TypeId;
        int rpcId = (1 << 24) | (0x03 << 22) | (entityTypeId << 8) | 3;

        // Build payload: [u32 seq][i64 ts]
        var w = new SpanWriter(64);
        try
        {
            w.WriteUInt32(0xCAFEBABEu);
            w.WriteInt64(0x1122334455667788L);
            var reader = new SpanReader(w.WrittenSpan);
            DefRpcDispatcher.DispatchBaseRpc(avatar, rpcId, System.IntPtr.Zero, ref reader);
        }
        finally { w.Dispose(); }

        Assert.Equal(1, ability.EchoCallCount);
        Assert.Equal(0xCAFEBABEu, ability.LastEchoSeq);
        Assert.Equal(0x1122334455667788L, ability.LastEchoTs);
    }

    [Fact]
    public void DispatchBaseRpc_UnknownSlot_DropsSilently()
    {
        var avatar = new AvatarWithAbility();
        avatar.AddComponent<AvatarAbility>();

        // Slot 99 is unknown — dispatcher walks _replicated, finds it
        // out of range, and bails without touching the entity.
        int rpcId = (99 << 24) | (0x03 << 22) | (avatar.TypeId << 8) | 3;
        var reader = new SpanReader(System.ReadOnlySpan<byte>.Empty);
        // Should not throw.
        DefRpcDispatcher.DispatchBaseRpc(avatar, rpcId, System.IntPtr.Zero, ref reader);
    }

    [Fact]
    public void DispatchBaseRpc_EmptySlot_DropsSilently()
    {
        // Component slot exists in the def but no AddComponent called.
        var avatar = new AvatarWithAbility();
        int rpcId = (1 << 24) | (0x03 << 22) | (avatar.TypeId << 8) | 3;
        var reader = new SpanReader(System.ReadOnlySpan<byte>.Empty);
        DefRpcDispatcher.DispatchBaseRpc(avatar, rpcId, System.IntPtr.Zero, ref reader);
        // Reaches here without exceptions — Echo was never invoked.
    }

    // Methods are sorted by name within each section: ApplyKv(1), ApplyTags(2),
    // Echo(3), EquipTestWeapon(4). The base_methods section direction is 0x03.

    [Fact]
    public void DispatchBaseRpc_ListArg_RoundTrips()
    {
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        int rpcId = (1 << 24) | (0x03 << 22) | (avatar.TypeId << 8) | 2;  // ApplyTags

        var w = new SpanWriter(64);
        try
        {
            // Wire format: [u16 count][int32 each]
            w.WriteUInt16(3);
            w.WriteInt32(11);
            w.WriteInt32(22);
            w.WriteInt32(33);
            var reader = new SpanReader(w.WrittenSpan);
            DefRpcDispatcher.DispatchBaseRpc(avatar, rpcId, System.IntPtr.Zero, ref reader);
        }
        finally { w.Dispose(); }

        Assert.NotNull(ability.LastTags);
        Assert.Equal(new[] { 11, 22, 33 }, ability.LastTags);
    }

    [Fact]
    public void DispatchBaseRpc_DictArg_RoundTrips()
    {
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        int rpcId = (1 << 24) | (0x03 << 22) | (avatar.TypeId << 8) | 1;  // ApplyKv

        var w = new SpanWriter(64);
        try
        {
            w.WriteUInt16(2);
            w.WriteString("fire");
            w.WriteInt32(50);
            w.WriteString("ice");
            w.WriteInt32(75);
            var reader = new SpanReader(w.WrittenSpan);
            DefRpcDispatcher.DispatchBaseRpc(avatar, rpcId, System.IntPtr.Zero, ref reader);
        }
        finally { w.Dispose(); }

        Assert.NotNull(ability.LastKv);
        Assert.Equal(2, ability.LastKv!.Count);
        Assert.Equal(50, ability.LastKv["fire"]);
        Assert.Equal(75, ability.LastKv["ice"]);
    }

    [Fact]
    public void DispatchBaseRpc_StructArg_RoundTrips()
    {
        var avatar = new AvatarWithAbility();
        var ability = avatar.AddComponent<AvatarAbility>();
        int rpcId = (1 << 24) | (0x03 << 22) | (avatar.TypeId << 8) | 4;  // EquipTestWeapon

        var src = new Atlas.Def.TestWeapon { Id = 999, Sharpness = 50, Bound = true };
        var w = new SpanWriter(64);
        try
        {
            src.Serialize(ref w);
            var reader = new SpanReader(w.WrittenSpan);
            DefRpcDispatcher.DispatchBaseRpc(avatar, rpcId, System.IntPtr.Zero, ref reader);
        }
        finally { w.Dispose(); }

        Assert.Equal(999, ability.LastWeapon.Id);
        Assert.Equal((ushort)50, ability.LastWeapon.Sharpness);
        Assert.True(ability.LastWeapon.Bound);
    }
}
}
