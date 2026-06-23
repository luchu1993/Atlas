using System;
using Atlas.Client.Native;
using Xunit;

namespace Atlas.Client.Tests;

public sealed class NativePredictorParityTests
{
    [Fact]
    public unsafe void NativeMovementPredictStepRunsShared10kSequence()
    {
        Assert.Equal(AtlasNetNative.AbiVersion, AtlasNetNative.AtlasNetGetAbiVersion());

        var state = new AtlasMovementStateFrame();
        uint rng = 0xA71A5001u;

        for (uint tick = 1; tick <= 10000; ++tick)
        {
            var input = new AtlasMovementInputFrame
            {
                Seq = tick,
                InputTick = tick,
                MoveX = QuantizedAxis(NextRandom(ref rng)),
                MoveZ = QuantizedAxis(NextRandom(ref rng)),
                ViewYaw = (ushort)(NextRandom(ref rng) & 0xFFFFu),
                ViewPitch = (sbyte)((NextRandom(ref rng) % 61u) - 30),
                Buttons = tick % 113u == 0u ? (ushort)1 : (ushort)0,
                ClientDtMs = 33,
            };

            var previous = state;
            AtlasMovementStateFrame next;
            int rc = AtlasNetNative.AtlasNetMovementPredictStep(&previous, &input, tick, &next);

            Assert.Equal(AtlasNetReturnCode.Ok, rc);
            Assert.True(IsFinite(next), $"native predictor produced non-finite state at {tick}");
            state = next;
        }

        Assert.Equal(10000u, state.LastProcessedInputSeq);
    }

    static uint NextRandom(ref uint state)
    {
        state = unchecked(state * 1664525u + 1013904223u);
        return state;
    }

    static sbyte QuantizedAxis(uint value) =>
        (sbyte)((int)((value >> 24) % 255u) - 127);

    static bool IsFinite(AtlasMovementStateFrame state) =>
        float.IsFinite(state.PositionX) &&
        float.IsFinite(state.PositionY) &&
        float.IsFinite(state.PositionZ) &&
        float.IsFinite(state.VelocityX) &&
        float.IsFinite(state.VelocityY) &&
        float.IsFinite(state.VelocityZ) &&
        float.IsFinite(state.DirectionX) &&
        float.IsFinite(state.DirectionY) &&
        float.IsFinite(state.DirectionZ);
}
