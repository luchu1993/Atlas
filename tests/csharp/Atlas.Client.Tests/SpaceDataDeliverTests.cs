using System;
using System.Collections.Generic;
using Atlas.Client;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Client.Tests
{
    // End-to-end probe: hand-crafted kSpaceData* envelopes pumped through
    // DeliverFromServer must land on SpaceDataManager byte-for-byte.
    public class SpaceDataDeliverTests : IDisposable
    {
        public SpaceDataDeliverTests() => ClientCallbacks.SpaceDataManager.ClearForTest();
        public void Dispose() => ClientCallbacks.SpaceDataManager.ClearForTest();

        private const ushort kClientReliableDeltaMessageId = 0xF003;
        private const byte kSpaceDataInit = 5;
        private const byte kSpaceDataUpdate = 6;
        private const byte kSpaceDataDelete = 7;

        private static byte[] BuildInit(uint spaceId, params (ushort key, byte[] value)[] entries)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kSpaceDataInit);
            w.WriteUInt32(spaceId);
            w.WriteUInt32((uint)entries.Length);
            foreach (var (k, v) in entries)
            {
                w.WriteUInt16(k);
                w.WriteUInt32((uint)v.Length);
                foreach (var b in v) w.WriteUInt8(b);
            }
            return w.WrittenSpan.ToArray();
        }

        private static byte[] BuildUpdate(uint spaceId, ushort key, byte[] value)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kSpaceDataUpdate);
            w.WriteUInt32(spaceId);
            w.WriteUInt16(key);
            w.WriteUInt32((uint)value.Length);
            foreach (var b in value) w.WriteUInt8(b);
            return w.WrittenSpan.ToArray();
        }

        private static byte[] BuildDelete(uint spaceId, ushort key)
        {
            var w = new SpanWriter();
            w.WriteUInt8(kSpaceDataDelete);
            w.WriteUInt32(spaceId);
            w.WriteUInt16(key);
            return w.WrittenSpan.ToArray();
        }

        [Fact]
        public void Init_PopulatesAllKeys()
        {
            var env = BuildInit(7, (1, new byte[] { 0xAA }), (2, new byte[] { 0xBB, 0xCC }));
            var fires = 0;
            ClientCallbacks.SpaceDataManager.Initialized += _ => fires++;

            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId, env);

            Assert.True(ClientCallbacks.SpaceDataManager.TryGet(7, 1, out var v1));
            Assert.Equal(new byte[] { 0xAA }, v1.ToArray());
            Assert.True(ClientCallbacks.SpaceDataManager.TryGet(7, 2, out var v2));
            Assert.Equal(new byte[] { 0xBB, 0xCC }, v2.ToArray());
            Assert.True(fires >= 1);
        }

        [Fact]
        public void Update_SetsAndFiresKeyChanged()
        {
            var seen = new List<(uint sp, ushort k)>();
            ClientCallbacks.SpaceDataManager.KeyChanged += (sp, k, _) => seen.Add((sp, k));

            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(11, 42, new byte[] { 0x01, 0x02 }));

            Assert.True(ClientCallbacks.SpaceDataManager.TryGet(11, 42, out var v));
            Assert.Equal(new byte[] { 0x01, 0x02 }, v.ToArray());
            Assert.Contains((11u, (ushort)42), seen);
        }

        [Fact]
        public void Delete_RemovesAndFiresKeyRemoved()
        {
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(13, 99, new byte[] { 0x7F }));
            Assert.True(ClientCallbacks.SpaceDataManager.TryGet(13, 99, out _));

            var seen = new List<(uint sp, ushort k)>();
            ClientCallbacks.SpaceDataManager.KeyRemoved += (sp, k) => seen.Add((sp, k));

            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId, BuildDelete(13, 99));

            Assert.False(ClientCallbacks.SpaceDataManager.TryGet(13, 99, out _));
            Assert.Contains((13u, (ushort)99), seen);
        }

        [Fact]
        public void TypedGetters_RoundTripFromUpdateEnvelopes()
        {
            // Write each scalar via the wire encoding scripts emit (Atlas.Space.Space).
            var int32Bytes = new byte[] { 0xD2, 0x04, 0x00, 0x00 };  // 1234 LE
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(30, 100, int32Bytes));
            Assert.Equal(1234, ClientCallbacks.SpaceDataManager.GetInt32(30, 100));

            var floatBytes = BitConverter.GetBytes(3.5f);
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(30, 101, floatBytes));
            Assert.Equal(3.5f, ClientCallbacks.SpaceDataManager.GetFloat(30, 101));

            var boolBytes = new byte[] { 0x01 };
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(30, 102, boolBytes));
            Assert.True(ClientCallbacks.SpaceDataManager.GetBool(30, 102));

            var strBytes = System.Text.Encoding.UTF8.GetBytes("hello");
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(30, 103, strBytes));
            Assert.Equal("hello", ClientCallbacks.SpaceDataManager.GetString(30, 103));
        }

        [Fact]
        public void TypedGetters_FallbackWhenMissingOrTruncated()
        {
            // Key 999 never set.
            Assert.Equal(42, ClientCallbacks.SpaceDataManager.GetInt32(40, 999, fallback: 42));
            // Set a 2-byte payload but request int32 (needs 4) → fallback.
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(40, 50, new byte[] { 0x01, 0x02 }));
            Assert.Equal(99, ClientCallbacks.SpaceDataManager.GetInt32(40, 50, fallback: 99));
        }

        [Fact]
        public void Init_ReplacesPriorEntriesForSameSpace()
        {
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(20, 1, new byte[] { 0x10 }));
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId,
                                              BuildUpdate(20, 99, new byte[] { 0x20 }));

            var env = BuildInit(20, (5, new byte[] { 0x55 }));
            ClientCallbacks.DeliverFromServer(kClientReliableDeltaMessageId, env);

            Assert.False(ClientCallbacks.SpaceDataManager.TryGet(20, 1, out _));
            Assert.False(ClientCallbacks.SpaceDataManager.TryGet(20, 99, out _));
            Assert.True(ClientCallbacks.SpaceDataManager.TryGet(20, 5, out var v));
            Assert.Equal(new byte[] { 0x55 }, v.ToArray());
        }
    }
}
