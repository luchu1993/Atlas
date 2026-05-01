using Atlas.Client;
using Atlas.DataTypes;
using Xunit;

namespace Atlas.Client.Tests
{
    public class AvatarFilterTests
    {
        private static AvatarFilter Make(double interval = 0.1)
        {
            return new AvatarFilter
            {
                LatencyFrames = 3.0,
                ServerInterval = interval,
                CurvePower = 2.0,
                MaxExtrapolation = 0.05,
            };
        }

        [Fact]
        public void EmptyBufferEvaluatesFalse()
        {
            var f = Make();
            Assert.False(f.TryEvaluate(0, out _, out _, out _));
        }

        [Fact]
        public void SingleSampleClampsToInput()
        {
            var f = Make();
            f.Input(0.0, new Vector3(1, 2, 3), Vector3.Forward, onGround: true);
            Assert.True(f.TryEvaluate(0.05, out var pos, out _, out var onGround));
            Assert.Equal(1f, pos.X);
            Assert.True(onGround);
        }

        [Fact]
        public void OutOfOrderInputDropped()
        {
            var f = Make();
            f.Input(1.0, new Vector3(10, 0, 0), Vector3.Right, false);
            f.Input(0.5, new Vector3(99, 99, 99), Vector3.Right, false);
            Assert.Equal(1, f.SampleCount);
        }

        [Fact]
        public void RingOverwritesOldestPastCapacity()
        {
            var f = Make();
            for (int i = 0; i < AvatarFilter.RingCapacity + 4; ++i)
                f.Input(i * 0.1, new Vector3(i, 0, 0), Vector3.Right, false);
            Assert.Equal(AvatarFilter.RingCapacity, f.SampleCount);
        }

        [Fact]
        public void InterpolatesBetweenBracketingSamples()
        {
            var f = Make();
            f.Input(0.0, new Vector3(0, 0, 0), Vector3.Forward, false);
            f.Input(0.1, new Vector3(10, 0, 0), Vector3.Forward, false);

            double clientTime = 0.05 + f.CurrentLatency;
            Assert.True(f.TryEvaluate(clientTime, out var pos, out _, out _));
            Assert.InRange(pos.X, 4.99f, 5.01f);
        }

        [Fact]
        public void LatencyConvergesTowardsTarget()
        {
            var f = Make(interval: 0.1);
            f.Input(0.0, Vector3.Zero, Vector3.Forward, false);
            double startLatency = f.CurrentLatency;
            f.LatencyFrames = 6.0;
            for (int i = 0; i < 2000; ++i) f.UpdateLatency(0.016);
            Assert.InRange(f.CurrentLatency, f.TargetLatency - 0.01, f.TargetLatency + 0.01);
            Assert.NotEqual(startLatency, f.CurrentLatency);
        }

        [Fact]
        public void ExtrapolationRespectsCap()
        {
            var f = Make();
            f.Input(0.0, Vector3.Zero, Vector3.Forward, false);
            f.Input(0.1, new Vector3(100, 0, 0), Vector3.Forward, false);

            // Past MaxExtrapolation cap: vel × cap = 1000 × 0.05 = 50 → max X ≈ 150.
            double futureClient = 1.0 + f.CurrentLatency;
            Assert.True(f.TryEvaluate(futureClient, out var pos, out _, out _));
            Assert.InRange(pos.X, 100f, 151f);
        }

        [Fact]
        public void ResetClearsState()
        {
            var f = Make();
            f.Input(0.0, Vector3.One, Vector3.Forward, false);
            f.Input(0.1, Vector3.One * 2, Vector3.Forward, false);
            f.Reset();
            Assert.Equal(0, f.SampleCount);
            Assert.False(f.TryEvaluate(0.05, out _, out _, out _));
        }
    }
}
