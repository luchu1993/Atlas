using System;
using Atlas.DataTypes;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Client.Tests
{
    public class ClientSessionTests
    {
        private const byte kEntityEnter = 1;
        private const byte kEntityPositionBatch = 8;
        private const byte kSpaceDataUpdate = 6;
        private const byte kPositionBatchHasTimeOffsets = 0x01;
        private const byte kPositionBatchHasOnGroundBits = 0x02;
        private const byte kPositionBatchAllOnGround = 0x04;
        private const byte kPositionBatchHasYOffsets = 0x08;
        private const byte kPositionBatchHasWideXZOffsets = 0x10;
        private const byte kPositionBatchSequentialEntityIds = 0x20;
        private const int kPositionBatchFlagsOffset = 1 + 4 + 3 * 4 + 8 + 2;

        private sealed class FakePeer : ClientEntity
        {
            public override string TypeName => "FakePeer";
            public override ushort TypeId => 777;
            public bool Initialized;
            public int PositionUpdates;

            protected internal override void OnInit()
            {
                Initialized = true;
            }

            protected internal override void OnPositionUpdated(Vector3 newPos)
            {
                ++PositionUpdates;
            }

            public void SendBaseForTest()
            {
                SendBaseRpc(42, ReadOnlySpan<byte>.Empty);
            }
        }

        private static byte[] BuildEnter(uint entityId, ushort typeId)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kEntityEnter);
            w.WriteUInt32(entityId);
            w.WriteUInt16(typeId);
            w.WriteVector3(Vector3.Zero);
            w.WriteVector3(Vector3.Forward);
            w.WriteBool(true);
            w.WriteDouble(1.0);
            return w.WrittenSpan.ToArray();
        }

        private static byte[] BuildPositionBatch(Vector3 origin, double baseTime,
                                                 params (uint EntityId, short X, short Y, short Z,
                                                         byte Yaw, bool OnGround,
                                                         ushort TimeOffsetMs)[] entries)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kEntityPositionBatch);
            w.WriteUInt32(0);
            w.WriteVector3(origin);
            w.WriteDouble(baseTime);
            w.WriteUInt16((ushort)entries.Length);
            Array.Sort(entries, (a, b) => a.EntityId.CompareTo(b.EntityId));
            bool hasTimeOffsets = Array.Exists(entries, entry => entry.TimeOffsetMs != 0);
            bool hasYOffsets = Array.Exists(entries, entry => entry.Y != 0);
            bool hasWideXZOffsets = Array.Exists(entries, entry => !CanPackSigned12(entry.X)
                                                                  || !CanPackSigned12(entry.Z));
            bool allOnGround = Array.TrueForAll(entries, entry => entry.OnGround);
            bool anyOnGround = Array.Exists(entries, entry => entry.OnGround);
            bool hasOnGroundBits = anyOnGround && !allOnGround;
            bool sequentialEntityIds = true;
            uint previous = entries.Length > 0 ? entries[0].EntityId : 0;
            for (int i = 0; i < entries.Length; ++i)
            {
                uint entityDelta = i == 0 ? 0 : entries[i].EntityId - previous;
                sequentialEntityIds = sequentialEntityIds && entityDelta == (i == 0 ? 0u : 1u);
                previous = entries[i].EntityId;
            }

            byte flags = 0;
            if (hasTimeOffsets) flags |= kPositionBatchHasTimeOffsets;
            if (hasOnGroundBits) flags |= kPositionBatchHasOnGroundBits;
            if (allOnGround) flags |= kPositionBatchAllOnGround;
            if (hasYOffsets) flags |= kPositionBatchHasYOffsets;
            if (hasWideXZOffsets) flags |= kPositionBatchHasWideXZOffsets;
            if (sequentialEntityIds) flags |= kPositionBatchSequentialEntityIds;
            w.WriteUInt8(flags);
            previous = entries.Length > 0 ? entries[0].EntityId : 0;
            w.WritePackedUInt32(previous);
            if (hasOnGroundBits)
            {
                for (int baseIndex = 0; baseIndex < entries.Length; baseIndex += 8)
                {
                    byte bits = 0;
                    for (int bit = 0; bit < 8 && baseIndex + bit < entries.Length; ++bit)
                    {
                        if (entries[baseIndex + bit].OnGround) bits |= (byte)(1 << bit);
                    }
                    w.WriteUInt8(bits);
                }
            }
            for (int i = 0; i < entries.Length; ++i)
            {
                var entry = entries[i];
                uint entityDelta = i == 0 ? 0 : entry.EntityId - previous;
                if (!sequentialEntityIds) w.WritePackedUInt32(entityDelta);
                if (hasWideXZOffsets)
                {
                    w.WriteInt16(entry.X);
                    w.WriteInt16(entry.Z);
                }
                else
                {
                    WritePackedXZ(ref w, entry.X, entry.Z);
                }
                if (hasYOffsets) w.WriteInt16(entry.Y);
                w.WriteUInt8(entry.Yaw);
                if (hasTimeOffsets) w.WriteUInt16(entry.TimeOffsetMs);
                previous = entry.EntityId;
            }
            return w.WrittenSpan.ToArray();
        }

        private static bool CanPackSigned12(short value)
            => value >= -2048 && value <= 2047;

        private static void WritePackedXZ(ref SpanWriter w, short x, short z)
        {
            uint packed = (uint)((ushort)x & 0x0FFF) | ((uint)((ushort)z & 0x0FFF) << 12);
            w.WriteUInt8((byte)(packed & 0xFF));
            w.WriteUInt8((byte)((packed >> 8) & 0xFF));
            w.WriteUInt8((byte)((packed >> 16) & 0xFF));
        }

        private static byte[] BuildSpaceDataUpdate(uint spaceId, ushort keyId, byte value)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kSpaceDataUpdate);
            w.WriteUInt32(spaceId);
            w.WriteUInt16(keyId);
            w.WriteUInt32(1);
            w.WriteUInt8(value);
            return w.WrittenSpan.ToArray();
        }

        [Fact]
        public void SessionsDoNotShareEntityOrSpaceDataState()
        {
            var left = new ClientSession();
            var right = new ClientSession();
            left.EntityFactory.Register(777, () => new FakePeer());
            right.EntityFactory.Register(777, () => new FakePeer());

            left.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                   BuildEnter(100, 777));
            left.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                   BuildSpaceDataUpdate(1, 10, 0xAA));
            right.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                    BuildEnter(200, 777));

            Assert.NotNull(left.EntityManager.Get(100));
            Assert.Null(right.EntityManager.Get(100));
            Assert.NotNull(right.EntityManager.Get(200));
            Assert.False(right.SpaceDataManager.TryGet(1, 10, out _));
            Assert.True(left.SpaceDataManager.TryGet(1, 10, out var value));
            Assert.Equal(new byte[] { 0xAA }, value.ToArray());
        }

        [Fact]
        public void ResetClearsRuntimeStateButKeepsRegistrations()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildSpaceDataUpdate(1, 10, 0xAA));

            session.Reset();

            Assert.Null(session.EntityManager.Get(100));
            Assert.False(session.SpaceDataManager.TryGet(1, 10, out _));

            session.CreateEntity(101, 777);

            Assert.NotNull(session.EntityManager.Get(101));
        }

        [Fact]
        public void OwnerEntityAddedFiresAfterOnInit()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            bool? initializedWhenAdded = null;
            session.EntityManager.EntityAdded += entity =>
                initializedWhenAdded = ((FakePeer)entity).Initialized;

            session.CreateEntity(100, 777);

            Assert.Equal(true, initializedWhenAdded);
        }

        [Fact]
        public void SessionEntityRpcRequiresSessionHandler()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.CreateEntity(100, 777);

            var entity = (FakePeer)session.EntityManager.Get(100)!;

            Assert.Throws<InvalidOperationException>(() => entity.SendBaseForTest());
        }

        [Fact]
        public void EntityRpcUsesOwningSessionHandler()
        {
            var left = new ClientSession();
            var right = new ClientSession();
            left.EntityFactory.Register(777, () => new FakePeer());
            right.EntityFactory.Register(777, () => new FakePeer());
            uint leftEntity = 0;
            uint rightEntity = 0;
            left.SendBaseRpcHandler = (entityId, _, _, _) => leftEntity = entityId;
            right.SendBaseRpcHandler = (entityId, _, _, _) => rightEntity = entityId;

            left.CreateEntity(100, 777);
            right.CreateEntity(200, 777);
            ((FakePeer)left.EntityManager.Get(100)!).SendBaseForTest();
            ((FakePeer)right.EntityManager.Get(200)!).SendBaseForTest();

            Assert.Equal(100u, leftEntity);
            Assert.Equal(200u, rightEntity);
        }

        [Fact]
        public void PositionBatchUpdatesKnownEntitiesAndIgnoresUnknownEntities()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(200, 777));

            var batch = BuildPositionBatch(
                new Vector3(10.0f, 1.0f, -5.0f), 10.0,
                (100, 123, 0, 50, 0, true, 250),
                (200, -25, 10, -75, 64, false, 500),
                (999, 1, 2, 3, 0, true, 1000));
            session.DeliverFromServer(ClientCallbacks.kClientDeltaMessageId, batch);

            var first = (FakePeer)session.EntityManager.Get(100)!;
            var second = (FakePeer)session.EntityManager.Get(200)!;
            Assert.InRange(first.Position.X, 11.229f, 11.231f);
            Assert.InRange(first.Position.Y, 0.999f, 1.001f);
            Assert.InRange(first.Position.Z, -4.501f, -4.499f);
            Assert.Equal(Vector3.Forward, first.Direction);
            Assert.True(first.OnGround);
            Assert.Equal(10.25, first.LastPositionServerTime);
            Assert.Equal(2, first.PositionUpdates);
            Assert.InRange(second.Position.X, 9.749f, 9.751f);
            Assert.InRange(second.Position.Y, 1.099f, 1.101f);
            Assert.InRange(second.Position.Z, -5.751f, -5.749f);
            Assert.InRange(second.Direction.X, 0.9999f, 1.0001f);
            Assert.InRange(second.Direction.Z, -0.0001f, 0.0001f);
            Assert.False(second.OnGround);
            Assert.Equal(10.5, second.LastPositionServerTime);
            Assert.Equal(2, second.PositionUpdates);
            Assert.Null(session.EntityManager.Get(999));
        }

        [Fact]
        public void PositionBatchDecodesUniformFields()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));

            var batch = BuildPositionBatch(
                new Vector3(10.0f, 1.0f, -5.0f), 12.0,
                (100, 10, 0, -20, 0, true, 0));
            session.DeliverFromServer(ClientCallbacks.kClientDeltaMessageId, batch);

            var entity = (FakePeer)session.EntityManager.Get(100)!;
            Assert.InRange(entity.Position.X, 10.099f, 10.101f);
            Assert.InRange(entity.Position.Y, 0.999f, 1.001f);
            Assert.InRange(entity.Position.Z, -5.201f, -5.199f);
            Assert.True(entity.OnGround);
            Assert.Equal(12.0, entity.LastPositionServerTime);
            Assert.Equal(2, entity.PositionUpdates);
        }

        [Fact]
        public void PositionBatchDecodesWideXZFields()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));

            var batch = BuildPositionBatch(
                Vector3.Zero, 12.0,
                (100, 3000, 0, -3000, 0, true, 0));
            session.DeliverFromServer(ClientCallbacks.kClientDeltaMessageId, batch);

            var entity = (FakePeer)session.EntityManager.Get(100)!;
            Assert.InRange(entity.Position.X, 29.999f, 30.001f);
            Assert.InRange(entity.Position.Z, -30.001f, -29.999f);
            Assert.True(entity.OnGround);
            Assert.Equal(12.0, entity.LastPositionServerTime);
        }

        [Fact]
        public void PositionBatchDecodesSequentialEntityIds()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(101, 777));

            var batch = BuildPositionBatch(
                Vector3.Zero, 20.0,
                (100, 100, 0, 200, 0, true, 0),
                (101, -100, 0, -200, 64, true, 0));
            session.DeliverFromServer(ClientCallbacks.kClientDeltaMessageId, batch);

            var first = (FakePeer)session.EntityManager.Get(100)!;
            var second = (FakePeer)session.EntityManager.Get(101)!;
            Assert.InRange(first.Position.X, 0.999f, 1.001f);
            Assert.InRange(first.Position.Z, 1.999f, 2.001f);
            Assert.Equal(2, first.PositionUpdates);
            Assert.InRange(second.Position.X, -1.001f, -0.999f);
            Assert.InRange(second.Position.Z, -2.001f, -1.999f);
            Assert.InRange(second.Direction.X, 0.9999f, 1.0001f);
            Assert.Equal(2, second.PositionUpdates);
        }

        [Fact]
        public void PositionBatchWithUnknownFlagsIsIgnored()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));

            var batch = BuildPositionBatch(
                Vector3.Zero, 20.0,
                (100, 100, 0, 200, 0, true, 0));
            batch[kPositionBatchFlagsOffset] |= 0x80;
            session.DeliverFromServer(ClientCallbacks.kClientDeltaMessageId, batch);

            var entity = (FakePeer)session.EntityManager.Get(100)!;
            Assert.Equal(Vector3.Zero, entity.Position);
            Assert.Equal(1, entity.PositionUpdates);
        }
    }
}
