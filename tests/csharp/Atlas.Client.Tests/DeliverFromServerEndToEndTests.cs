using Atlas.Client;
using Atlas.DataTypes;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Client.Tests
{
    // End-to-end probe: constructs 0xF001 AoI envelopes by hand and pushes them
    // through the same DeliverFromServer entry point that net_client.dll's
    // on_deliver callback hits in production. Proves the wire layout decoded by
    // ClientCallbacks matches witness.cc's BuildEnterEnvelope / SendEntityUpdate
    // *and* that ClientEntity wires the AvatarFilter on the peer path.
    public class DeliverFromServerEndToEndTests
    {
        private const ushort kClientReliableDeltaMessageId = 0xF003;
        private const ushort kClientDeltaMessageId = 0xF001;
        private const byte kEntityEnter = 1;
        private const byte kEntityPositionUpdate = 3;

        private sealed class FakePeer : ClientEntity
        {
            public override string TypeName => "FakePeer";
            public override ushort TypeId => 999;
        }

        public DeliverFromServerEndToEndTests()
        {
            // Entity factory + manager are static singletons; reset before each test
            // by destroying anything left from a previous run.
            ClientEntityFactory.Register(999, () => new FakePeer());
            var leftovers = ClientCallbacks.EntityManager;
            // Walk a snapshot list to avoid mutating during enumeration.
            foreach (var e in System.Linq.Enumerable.ToList(leftovers.Entities))
                leftovers.Destroy(e.EntityId);
        }

        // [u8 kind=1][u32 eid][u16 typeId][3f pos][3f dir][u8 og][f64 serverTime][peerSnapshot].
        private static byte[] BuildEnterEnvelope(uint eid, ushort typeId,
                                                 Vector3 pos, Vector3 dir,
                                                 bool onGround, double serverTime)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kEntityEnter);
            w.WriteUInt32(eid);
            w.WriteUInt16(typeId);
            w.WriteVector3(pos);
            w.WriteVector3(dir);
            w.WriteBool(onGround);
            w.WriteDouble(serverTime);
            return w.WrittenSpan.ToArray();
        }

        // [u8 kind=3][u32 eid][3f pos][3f dir][u8 og][f64 serverTime].
        private static byte[] BuildPositionEnvelope(uint eid, Vector3 pos, Vector3 dir,
                                                    bool onGround, double serverTime)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kEntityPositionUpdate);
            w.WriteUInt32(eid);
            w.WriteVector3(pos);
            w.WriteVector3(dir);
            w.WriteBool(onGround);
            w.WriteDouble(serverTime);
            return w.WrittenSpan.ToArray();
        }

        [Fact]
        public void EnterPlusPositionUpdateFeedsAvatarFilter()
        {
            const uint eid = 1234;
            var enter = BuildEnterEnvelope(eid, typeId: 999,
                                           pos: new Vector3(0, 0, 0),
                                           dir: Vector3.Forward,
                                           onGround: true,
                                           serverTime: 100.0);
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId, enter);

            var entity = ClientCallbacks.EntityManager.Get(eid);
            Assert.NotNull(entity);
            Assert.False(entity!.IsOwner);
            Assert.NotNull(entity.Filter);
            Assert.Equal(1, entity.Filter!.SampleCount);
            Assert.Equal(100.0, entity.LastPositionServerTime);

            // Volatile update at later serverTime; envelope rides 0xF001.
            var move = BuildPositionEnvelope(eid,
                                             pos: new Vector3(10, 0, 0),
                                             dir: Vector3.Forward,
                                             onGround: true,
                                             serverTime: 100.1);
            ClientCallbacks.DeliverFromServer(kClientDeltaMessageId, move);

            Assert.Equal(2, entity.Filter!.SampleCount);
            Assert.Equal(100.1, entity.LastPositionServerTime);

            // Pull the interpolated transform mid-window: clientTime - latencyTarget
            // (3 frames * 0.1s = 0.3s by AvatarFilter defaults) puts us between samples.
            // Filter target latency = 0.3, so clientTime = 100.4 -> targetTime = 100.1
            // which clamps to newest. Use 100.35 for the midpoint.
            Assert.True(entity.TryGetInterpolated(100.35, out var pos, out _, out var og));
            Assert.InRange(pos.X, 4.5f, 5.5f);
            Assert.True(og);
        }

        [Fact]
        public void OutOfOrderPositionUpdateDropped()
        {
            const uint eid = 5555;
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                BuildEnterEnvelope(eid, 999, Vector3.Zero, Vector3.Forward, true, 50.0));
            ClientCallbacks.DeliverFromServer(kClientDeltaMessageId,
                BuildPositionEnvelope(eid, new Vector3(5, 0, 0), Vector3.Forward, true, 50.1));

            var entity = ClientCallbacks.EntityManager.Get(eid)!;
            Assert.Equal(2, entity.Filter!.SampleCount);

            // Stale serverTime — AvatarFilter.Input must drop it.
            ClientCallbacks.DeliverFromServer(kClientDeltaMessageId,
                BuildPositionEnvelope(eid, new Vector3(99, 99, 99), Vector3.Forward, true, 50.05));

            Assert.Equal(2, entity.Filter!.SampleCount);
        }
    }
}
