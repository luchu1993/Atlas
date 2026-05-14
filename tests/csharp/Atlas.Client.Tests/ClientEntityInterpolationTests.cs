using Atlas.Client;
using Atlas.DataTypes;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Client.Tests
{
    public class ClientEntityInterpolationTests
    {
        private sealed class TestEntity : ClientEntity
        {
            public override string TypeName => "Test";
        }

        private sealed class Clock
        {
            public double Now;
        }

        [Fact]
        public void PeerEntityFeedsFilterAndInterpolates()
        {
            var clock = new Clock { Now = 1.00 };
            // Pre-attach so ApplyPositionUpdate doesn't lazy-create with default wall.
            var e = new TestEntity { EntityId = 100, Filter = new AvatarFilter(() => clock.Now) };
            Assert.False(e.IsOwner);

            e.ApplyPositionUpdate(serverTime: 1.00,
                                  pos: new Vector3(0, 0, 0),
                                  dir: Vector3.Forward,
                                  onGround: true);
            Assert.Equal(1, e.Filter!.SampleCount);

            clock.Now = 1.10;
            e.ApplyPositionUpdate(serverTime: 1.10,
                                  pos: new Vector3(10, 0, 0),
                                  dir: Vector3.Forward,
                                  onGround: true);
            Assert.Equal(2, e.Filter!.SampleCount);

            // wall=1.35, offset stays 0 (clock tracks server), latency=0.3
            // → targetTime=1.05, midway between the two samples.
            clock.Now = 1.35;
            Assert.True(e.TryGetInterpolated(out var pos, out _, out _));
            Assert.InRange(pos.X, 4.5f, 5.5f);
        }

        [Fact]
        public void OwnerEntitySkipsFilter()
        {
            var e = new TestEntity { EntityId = 1, IsOwner = true };
            e.ApplyPositionUpdate(2.00, new Vector3(7, 0, 0), Vector3.Forward, true);
            e.ApplyPositionUpdate(2.10, new Vector3(8, 0, 0), Vector3.Forward, true);
            Assert.Null(e.Filter);

            // No filter -> TryGetInterpolated returns false but still surfaces last snapshot.
            Assert.False(e.TryGetInterpolated(out var pos, out _, out _));
            Assert.Equal(8f, pos.X);
        }

        [Fact]
        public void ManagerTickInterpolationAdvancesLatencyConvergence()
        {
            var mgr = new ClientEntityManager();
            var e = new TestEntity { EntityId = 5 };
            mgr.Register(e);

            // First sample latches CurrentLatency = TargetLatency (0.3 by default).
            e.ApplyPositionUpdate(0.0, Vector3.Zero, Vector3.Forward, true);
            var initial = e.Filter!.CurrentLatency;

            // Move the target so the convergence loop has work to do.
            e.Filter.LatencyFrames = 6.0;  // TargetLatency = 0.6
            for (int i = 0; i < 60; ++i) mgr.TickInterpolation(1f / 60f);

            Assert.True(e.Filter!.CurrentLatency > initial,
                        $"latency should converge upward; initial={initial}, after={e.Filter!.CurrentLatency}");
        }

        [Fact]
        public void ResetInterpolationDropsRingButKeepsLastSnapshot()
        {
            var e = new TestEntity { EntityId = 7 };
            e.ApplyPositionUpdate(1.0, new Vector3(5, 0, 0), Vector3.Forward, true);
            e.ApplyPositionUpdate(1.1, new Vector3(6, 0, 0), Vector3.Forward, true);
            Assert.Equal(2, e.Filter!.SampleCount);

            e.ResetInterpolation();
            Assert.Equal(0, e.Filter!.SampleCount);
            Assert.Equal(6f, e.Position.X);
        }
    }
}
