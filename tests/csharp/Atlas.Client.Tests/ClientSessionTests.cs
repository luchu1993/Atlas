using System;
using Atlas.DataTypes;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Client.Tests
{
    public class ClientSessionTests
    {
        private const byte kEntityEnter = 1;
        private const byte kSpaceDataUpdate = 6;

        private sealed class FakePeer : ClientEntity
        {
            public override string TypeName => "FakePeer";
            public override ushort TypeId => 777;
            public bool Initialized;

            protected internal override void OnInit()
            {
                Initialized = true;
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
    }
}
