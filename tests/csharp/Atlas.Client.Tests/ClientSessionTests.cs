using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Atlas.Client.Native;
using Atlas.Components;
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

        [Fact]
        public void MovementNativeStructLayoutsStayPinned()
        {
            Assert.Equal(17, Marshal.SizeOf<AtlasMovementInputFrame>());
            Assert.Equal(44, Marshal.SizeOf<AtlasMovementStateFrame>());
            Assert.Equal(15, Marshal.OffsetOf<AtlasMovementInputFrame>(
                nameof(AtlasMovementInputFrame.ClientDtMs)).ToInt32());
            Assert.Equal(40, Marshal.OffsetOf<AtlasMovementStateFrame>(
                nameof(AtlasMovementStateFrame.LastProcessedInputSeq)).ToInt32());
        }

        private sealed class FakePeer : ClientEntity
        {
            public override string TypeName => "FakePeer";
            public override ushort TypeId => 777;
            public bool Initialized;
            public int PositionUpdates;
            public TickProbeComponent? TickProbe => GetLocalComponent<TickProbeComponent>();

            protected internal override void OnInit()
            {
                Initialized = true;
                AddLocalComponent<TickProbeComponent>();
            }

            protected internal override void OnPositionUpdated(Vector3 newPos)
            {
                ++PositionUpdates;
            }

            public void SendBaseForTest()
            {
                SendBaseRpc(42, ReadOnlySpan<byte>.Empty);
            }

            public void SendMovementForTest(AtlasMovementInputFrame frame)
            {
                SendMovementInput(new[] { frame });
            }

            public void ReportMovementCorrectionForTest()
            {
                SendMovementCorrectionReport(42, 9001, 0.5f, MovementCorrection.Tier1Flag);
            }
        }

        private sealed class TickProbeComponent : ClientLocalComponent
        {
            public int Ticks;
            public int Detaches;
            public float LastDeltaTime;

            public override void OnTick(float deltaTime)
            {
                ++Ticks;
                LastDeltaTime = deltaTime;
            }

            public override void OnDetached()
            {
                ++Detaches;
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

        private static byte[] BuildSpaceBspGeometry()
        {
            var w = new SpanWriter();
            w.WriteUInt32(42);
            w.WritePackedUInt32(1);
            w.WriteUInt32(7);
            w.WriteUInt8(3);
            w.WriteFloat(-10.0f);
            w.WriteFloat(-20.0f);
            w.WriteFloat(30.0f);
            w.WriteFloat(40.0f);
            w.WriteFloat(0.625f);
            w.WriteUInt32(123);
            return w.WrittenSpan.ToArray();
        }

        private static byte[] BuildMovementStateAck()
        {
            var w = new SpanWriter();
            w.WriteUInt32(100);
            w.WriteUInt32(42);
            w.WriteUInt32(9001);
            w.WriteVector3(new Vector3(1.0f, 2.0f, 3.0f));
            w.WriteVector3(new Vector3(4.0f, 5.0f, 6.0f));
            w.WriteVector3(Vector3.Forward);
            w.WriteUInt32(1);
            w.WriteUInt32(42);
            w.WriteUInt16(MovementCorrection.SnapFlag);
            return w.WrittenSpan.ToArray();
        }

        private static byte[] BuildMovementCommandStart()
        {
            var w = new SpanWriter();
            w.WriteUInt32(100);
            w.WriteUInt32(900);
            w.WriteUInt16(12);
            w.WriteUInt8((byte)MovementCommandType.Knockback);
            w.WriteVector3(new Vector3(1.0f, 2.0f, 3.0f));
            w.WriteVector3(new Vector3(4.0f, 5.0f, 6.0f));
            w.WriteUInt16(700);
            w.WriteUInt16(100);
            w.WriteUInt16(3);
            w.WriteUInt8((byte)MovementCommandInputPolicy.AllowTurn);
            w.WriteUInt8((byte)MovementCommandCollisionPolicy.EndSkill);
            w.WriteUInt8(9);
            w.WriteUInt32(9001);
            return w.WrittenSpan.ToArray();
        }

        private static byte[] BuildMovementCommandEnd()
        {
            var w = new SpanWriter();
            w.WriteUInt32(100);
            w.WriteUInt32(900);
            w.WriteUInt32(9002);
            w.WriteUInt8((byte)MovementCommandEndReason.Collision);
            w.WriteVector3(new Vector3(4.0f, 5.0f, 6.0f));
            w.WriteVector3(new Vector3(0.0f, 0.0f, 0.0f));
            w.WriteVector3(Vector3.Forward);
            w.WriteUInt32(1);
            w.WriteUInt32(77);
            return w.WrittenSpan.ToArray();
        }

        private static byte[] AppendTrailingByte(byte[] body)
        {
            Array.Resize(ref body, body.Length + 1);
            body[^1] = 0xEE;
            return body;
        }

        private static void WriteQuietNaN(byte[] body, int offset)
        {
            body[offset] = 0;
            body[offset + 1] = 0;
            body[offset + 2] = 0xC0;
            body[offset + 3] = 0x7F;
        }

        private static void WriteZeroUInt32(byte[] body, int offset)
        {
            body[offset] = 0;
            body[offset + 1] = 0;
            body[offset + 2] = 0;
            body[offset + 3] = 0;
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
        public void SpaceBspGeometryDecodesLoadAndEntityCount()
        {
            var session = new ClientSession();
            IReadOnlyList<ClientCallbacks.BspLeafRect>? leaves = null;
            uint spaceId = 0;
            session.SpaceBspGeometryReceived += (sid, rects) =>
            {
                spaceId = sid;
                leaves = rects;
            };

            session.DeliverFromServer(ClientCallbacks.kSpaceBspGeometryMessageId,
                                      BuildSpaceBspGeometry());

            Assert.Equal(42u, spaceId);
            Assert.NotNull(leaves);
            var leaf = Assert.Single(leaves);
            Assert.Equal(7u, leaf.CellId);
            Assert.Equal((byte)3, leaf.OwnerIndex);
            Assert.Equal(-10.0f, leaf.MinX);
            Assert.Equal(40.0f, leaf.MaxZ);
            Assert.Equal(0.625f, leaf.Load);
            Assert.Equal(123u, leaf.EntityCount);
        }

        [Fact]
        public void MovementStateAckDispatchesEvent()
        {
            var session = new ClientSession();
            MovementStateAck? received = null;
            session.MovementStateAckReceived += ack => received = ack;

            session.DeliverFromServer(ClientCallbacks.kClientMovementStateAckMessageId,
                                      BuildMovementStateAck());

            Assert.True(received.HasValue);
            Assert.Equal(100u, received.Value.EntityId);
            Assert.Equal(42u, received.Value.AckedInputSeq);
            Assert.Equal(9001u, received.Value.ServerTick);
            Assert.Equal(1.0f, received.Value.State.Position.X);
            Assert.Equal(6.0f, received.Value.State.Velocity.Z);
            Assert.Equal(1u, received.Value.State.Flags);
            Assert.Equal(MovementCorrection.SnapFlag, received.Value.CorrectionFlags);
        }

        [Fact]
        public void MovementStateAckRejectsInvalidCorrectionFlags()
        {
            var session = new ClientSession();
            MovementStateAck? received = null;
            session.MovementStateAckReceived += ack => received = ack;
            const int flagsOffset = 4 + 4 + 4 + 36 + 4 + 4;

            var multiBit = BuildMovementStateAck();
            multiBit[flagsOffset] =
                (byte)(MovementCorrection.Tier1Flag | MovementCorrection.Tier2Flag);
            multiBit[flagsOffset + 1] = 0;
            session.DeliverFromServer(ClientCallbacks.kClientMovementStateAckMessageId, multiBit);
            Assert.False(received.HasValue);

            var reservedBit = BuildMovementStateAck();
            reservedBit[flagsOffset] = (byte)(1 << 5);
            reservedBit[flagsOffset + 1] = 0;
            session.DeliverFromServer(ClientCallbacks.kClientMovementStateAckMessageId, reservedBit);
            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementStateAckRejectsTrailingBytes()
        {
            var session = new ClientSession();
            MovementStateAck? received = null;
            session.MovementStateAckReceived += ack => received = ack;
            var body = AppendTrailingByte(BuildMovementStateAck());

            session.DeliverFromServer(ClientCallbacks.kClientMovementStateAckMessageId, body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementStateAckRejectsZeroEntityId()
        {
            var session = new ClientSession();
            MovementStateAck? received = null;
            session.MovementStateAckReceived += ack => received = ack;
            var body = BuildMovementStateAck();
            WriteZeroUInt32(body, 0);

            session.DeliverFromServer(ClientCallbacks.kClientMovementStateAckMessageId, body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementStateAckRejectsNonFiniteState()
        {
            var session = new ClientSession();
            MovementStateAck? received = null;
            session.MovementStateAckReceived += ack => received = ack;
            var body = BuildMovementStateAck();
            WriteQuietNaN(body, 12);

            session.DeliverFromServer(ClientCallbacks.kClientMovementStateAckMessageId, body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandStartDispatchesEvent()
        {
            var session = new ClientSession();
            MovementCommandStart? received = null;
            session.MovementCommandStarted += command => received = command;

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      BuildMovementCommandStart());

            Assert.True(received.HasValue);
            Assert.Equal(100u, received.Value.EntityId);
            Assert.Equal(900u, received.Value.Command.CommandId);
            Assert.Equal(MovementCommandType.Knockback, received.Value.Command.Type);
            Assert.Equal(12, received.Value.Command.SkillId);
            Assert.Equal(1.0f, received.Value.Command.StartPosition.X);
            Assert.Equal(6.0f, received.Value.Command.TargetPosition.Z);
            Assert.Equal(700, received.Value.Command.DurationMs);
            Assert.Equal(100, received.Value.Command.ElapsedMs);
            Assert.Equal(3, received.Value.Command.CurveId);
            Assert.Equal(MovementCommandInputPolicy.AllowTurn,
                         received.Value.Command.InputPolicy);
            Assert.Equal(MovementCommandCollisionPolicy.EndSkill,
                         received.Value.Command.CollisionPolicy);
            Assert.Equal(9, received.Value.Command.Priority);
            Assert.Equal(9001u, received.Value.Command.ServerTick);
        }

        [Fact]
        public void MovementCommandStartRejectsInvalidEnum()
        {
            var session = new ClientSession();
            MovementCommandStart? received = null;
            session.MovementCommandStarted += command => received = command;
            var body = BuildMovementCommandStart();
            body[10] = 255;

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId, body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandStartRejectsZeroEntityId()
        {
            var session = new ClientSession();
            MovementCommandStart? received = null;
            session.MovementCommandStarted += command => received = command;
            var body = BuildMovementCommandStart();
            WriteZeroUInt32(body, 0);

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandStartRejectsZeroCommandId()
        {
            var session = new ClientSession();
            MovementCommandStart? received = null;
            session.MovementCommandStarted += command => received = command;
            var body = BuildMovementCommandStart();
            WriteZeroUInt32(body, 4);

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandStartRejectsNonFinitePosition()
        {
            var session = new ClientSession();
            MovementCommandStart? received = null;
            session.MovementCommandStarted += command => received = command;
            var body = BuildMovementCommandStart();
            WriteQuietNaN(body, 11);

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandStartRejectsInvalidTiming()
        {
            var session = new ClientSession();
            MovementCommandStart? received = null;
            session.MovementCommandStarted += command => received = command;
            var zeroDuration = BuildMovementCommandStart();
            zeroDuration[35] = 0;
            zeroDuration[36] = 0;

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      zeroDuration);

            Assert.False(received.HasValue);

            var elapsedPastDuration = BuildMovementCommandStart();
            elapsedPastDuration[37] = 0xBD;
            elapsedPastDuration[38] = 0x02;

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      elapsedPastDuration);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandStartRejectsTrailingBytes()
        {
            var session = new ClientSession();
            MovementCommandStart? received = null;
            session.MovementCommandStarted += command => received = command;
            var body = AppendTrailingByte(BuildMovementCommandStart());

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandEndDispatchesEvent()
        {
            var session = new ClientSession();
            MovementCommandEnd? received = null;
            session.MovementCommandEnded += command => received = command;

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandEndMessageId,
                                      BuildMovementCommandEnd());

            Assert.True(received.HasValue);
            Assert.Equal(100u, received.Value.EntityId);
            Assert.Equal(900u, received.Value.CommandId);
            Assert.Equal(9002u, received.Value.ServerTick);
            Assert.Equal(MovementCommandEndReason.Collision, received.Value.Reason);
            Assert.Equal(4.0f, received.Value.State.Position.X);
            Assert.Equal(6.0f, received.Value.State.Position.Z);
            Assert.Equal(77u, received.Value.State.LastProcessedInputSeq);
        }

        [Fact]
        public void MovementCommandEndRejectsInvalidReason()
        {
            var session = new ClientSession();
            MovementCommandEnd? received = null;
            session.MovementCommandEnded += command => received = command;
            var body = BuildMovementCommandEnd();
            body[12] = 255;

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandEndMessageId, body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandEndRejectsZeroEntityId()
        {
            var session = new ClientSession();
            MovementCommandEnd? received = null;
            session.MovementCommandEnded += command => received = command;
            var body = BuildMovementCommandEnd();
            WriteZeroUInt32(body, 0);

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandEndMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandEndRejectsZeroCommandId()
        {
            var session = new ClientSession();
            MovementCommandEnd? received = null;
            session.MovementCommandEnded += command => received = command;
            var body = BuildMovementCommandEnd();
            WriteZeroUInt32(body, 4);

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandEndMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandEndRejectsNonFiniteState()
        {
            var session = new ClientSession();
            MovementCommandEnd? received = null;
            session.MovementCommandEnded += command => received = command;
            var body = BuildMovementCommandEnd();
            WriteQuietNaN(body, 13);

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandEndMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandEndRejectsTrailingBytes()
        {
            var session = new ClientSession();
            MovementCommandEnd? received = null;
            session.MovementCommandEnded += command => received = command;
            var body = AppendTrailingByte(BuildMovementCommandEnd());

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandEndMessageId,
                                      body);

            Assert.False(received.HasValue);
        }

        [Fact]
        public void MovementCommandStartAppliesRemoteEntity()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      BuildMovementCommandStart());

            var entity = (FakePeer)session.EntityManager.Get(100)!;
            Assert.True(entity.HasActiveMovementCommand);
            Assert.True(entity.TryGetInterpolated(out var pos, out var dir, out _));
            Assert.InRange(pos.X, 1.42f, 1.44f);
            Assert.InRange(pos.Z, 3.42f, 3.44f);
            Assert.InRange(dir.X, 0.57f, 0.58f);
            Assert.Equal(0, entity.Filter!.SampleCount);
        }

        [Fact]
        public void MovementCommandEndAppliesRemoteEntity()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.DeliverFromServer(ClientCallbacks.kClientReliableDeltaMessageId,
                                      BuildEnter(100, 777));
            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandStartMessageId,
                                      BuildMovementCommandStart());

            session.DeliverFromServer(ClientCallbacks.kClientMovementCommandEndMessageId,
                                      BuildMovementCommandEnd());

            var entity = (FakePeer)session.EntityManager.Get(100)!;
            Assert.False(entity.HasActiveMovementCommand);
            Assert.True(entity.TryGetInterpolated(out var pos, out var dir, out var onGround));
            Assert.Equal(4.0f, pos.X);
            Assert.Equal(6.0f, pos.Z);
            Assert.Equal(1.0f, dir.Z);
            Assert.True(onGround);
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
        public void EntityMovementUsesOwningSessionHandler()
        {
            var left = new ClientSession();
            var right = new ClientSession();
            left.EntityFactory.Register(777, () => new FakePeer());
            right.EntityFactory.Register(777, () => new FakePeer());
            uint leftInputEntity = 0;
            uint rightInputEntity = 0;
            uint leftCorrectionEntity = 0;
            uint rightCorrectionTick = 0;
            left.SendMovementInputHandler = (entityId, frames) =>
            {
                leftInputEntity = entityId;
                Assert.Equal(1, frames.Length);
            };
            right.SendMovementInputHandler = (entityId, frames) =>
            {
                rightInputEntity = entityId;
                Assert.Equal(7u, frames[0].Seq);
            };
            left.SendMovementCorrectionReportHandler = (entityId, _, _, _, _) =>
                leftCorrectionEntity = entityId;
            right.SendMovementCorrectionReportHandler = (_, _, serverTick, _, _) =>
                rightCorrectionTick = serverTick;

            left.CreateEntity(100, 777);
            right.CreateEntity(200, 777);
            var frame = new AtlasMovementInputFrame { Seq = 7, ClientDtMs = 33 };
            ((FakePeer)left.EntityManager.Get(100)!).SendMovementForTest(frame);
            ((FakePeer)right.EntityManager.Get(200)!).SendMovementForTest(frame);
            ((FakePeer)left.EntityManager.Get(100)!).ReportMovementCorrectionForTest();
            ((FakePeer)right.EntityManager.Get(200)!).ReportMovementCorrectionForTest();

            Assert.Equal(100u, leftInputEntity);
            Assert.Equal(200u, rightInputEntity);
            Assert.Equal(100u, leftCorrectionEntity);
            Assert.Equal(9001u, rightCorrectionTick);
        }

        [Fact]
        public void SessionTickDrivesClientLocalComponents()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.CreateEntity(100, 777);

            var entity = (FakePeer)session.EntityManager.Get(100)!;
            session.Tick(0.25f);

            Assert.NotNull(entity.TickProbe);
            Assert.Equal(1, entity.TickProbe.Ticks);
            Assert.Equal(0.25f, entity.TickProbe.LastDeltaTime);
        }

        [Fact]
        public void DestroyDetachesClientLocalComponents()
        {
            var session = new ClientSession();
            session.EntityFactory.Register(777, () => new FakePeer());
            session.CreateEntity(100, 777);

            var entity = (FakePeer)session.EntityManager.Get(100)!;
            var probe = entity.TickProbe!;
            session.DestroyEntity(100);

            Assert.True(entity.IsDestroyed);
            Assert.Equal(1, probe.Detaches);
            Assert.Null(session.EntityManager.Get(100));
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
