using Atlas.Client;
using Atlas.DataTypes;
using Xunit;

namespace Atlas.Client.Tests
{
    // Locked-down scenarios with byte-identical expected outputs to the C++ port
    // (samples/mvp/UEClient/.../avatar_filter_test.cpp). Diverging here means the
    // C# and C++ AvatarFilter implementations drift apart at the float level.
    public class AvatarFilterParityTests
    {
        private sealed class Clock
        {
            public double Now;
        }

        private static AvatarFilter Make(Clock clock) =>
            new AvatarFilter(() => clock.Now)
            {
                LatencyFrames = 3.0,
                ServerInterval = 0.1,
                CurvePower = 2.0,
                MaxExtrapolation = 0.05,
            };

        // Mirrors C++ test #2: single sample at server=10.0, wall=10.0.
        // target_time = 10.0 - 0 - 0.3 = 9.7; count=1 extrapolation returns the sample as-is.
        [Fact]
        public void SingleSampleEchoes_ExactFloat()
        {
            var clock = new Clock { Now = 10.0 };
            var f = Make(clock);
            f.Input(10.0, new Vector3(1, 2, 3), new Vector3(1, 0, 0), onGround: true);
            clock.Now = 10.0;

            Assert.True(f.TryEvaluate(out var pos, out var dir, out var onGround));
            Assert.Equal(1.0f, pos.X);
            Assert.Equal(2.0f, pos.Y);
            Assert.Equal(3.0f, pos.Z);
            Assert.Equal(1.0f, dir.X);
            Assert.True(onGround);
        }

        // Mirrors C++ test #3: two samples 1.0 apart, target_time exactly 0.7 between them.
        // After Input(10.0,0): offset=0, latency=0.3. Then Input(11.0,10) at wall=11.0: offset
        // stays 0. TryEvaluate at wall=11.0 → target=10.7 → lerp t=0.7 → pos.x = 7.0 exact.
        [Fact]
        public void TwoSampleLerpAtTargetTime_ExactFloat()
        {
            var clock = new Clock { Now = 10.0 };
            var f = Make(clock);
            f.Input(10.0, Vector3.Zero, Vector3.Zero, false);
            clock.Now = 11.0;
            f.Input(11.0, new Vector3(10, 0, 0), Vector3.Zero, false);

            clock.Now = 11.0;
            Assert.True(f.TryEvaluate(out var pos, out _, out _));
            Assert.Equal(7.0f, pos.X);
            Assert.Equal(0.0f, pos.Y);
            Assert.Equal(0.0f, pos.Z);
        }

        // Mirrors C++ test #4: extrapolation cap at MaxExtrapolation = 0.05.
        // After 2 Inputs at wall=10.0: wall_offset = 0.05 * (10.0 - 10.1) = -0.005.
        // TryEvaluate at wall=10.5 → target = 10.205, ahead = 0.105 clamped to 0.05.
        // span = 0.1, scale = 0.5, pos.x = 1.0 + (1.0 - 0.0) * 0.5 = 1.5 exact.
        [Fact]
        public void ExtrapolationCap_ExactFloat()
        {
            var clock = new Clock { Now = 10.0 };
            var f = Make(clock);
            f.Input(10.0, Vector3.Zero, Vector3.Zero, false);
            f.Input(10.1, new Vector3(1, 0, 0), Vector3.Zero, false);

            clock.Now = 10.5;
            Assert.True(f.TryEvaluate(out var pos, out _, out _));
            Assert.Equal(1.5f, pos.X);
        }
    }
}
