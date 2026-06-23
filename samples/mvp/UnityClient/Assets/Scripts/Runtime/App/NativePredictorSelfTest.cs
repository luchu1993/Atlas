using System;
using Atlas.Client.Native;
using Atlas.Client.Unity;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    static class NativePredictorSelfTest
    {
        const string kArg = "-atlas-predictor-self-test";
        const uint kTicks = 10000;

        public static bool TryRunFromCommandLine()
        {
            if (!HasArg(Environment.GetCommandLineArgs(), kArg)) return false;

            int exitCode = 1;
            try
            {
                Run();
                Debug.Log($"[NativePredictorSelfTest] PASS ticks={kTicks}");
                exitCode = 0;
            }
            catch (Exception ex)
            {
                Debug.LogError($"[NativePredictorSelfTest] FAIL {ex}");
            }
            Application.Quit(exitCode);
            return true;
        }

        static void Run()
        {
            var state = new AtlasMovementStateFrame();
            uint rng = 0xA71A5001u;

            for (uint tick = 1; tick <= kTicks; ++tick)
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
                int rc = AtlasNetworkManager.PredictMovement(previous, input, tick, out var next);
                if (rc != AtlasNetReturnCode.Ok)
                    throw new InvalidOperationException(
                        $"predict step failed at tick {tick}: rc={rc}");
                if (!IsFinite(next))
                    throw new InvalidOperationException(
                        $"predict step produced non-finite state at tick {tick}");
                state = next;
            }

            if (state.LastProcessedInputSeq != kTicks)
                throw new InvalidOperationException(
                    $"last processed seq {state.LastProcessedInputSeq} != {kTicks}");
        }

        static bool HasArg(string[] args, string name)
        {
            foreach (string arg in args)
            {
                if (arg == name) return true;
            }
            return false;
        }

        static uint NextRandom(ref uint state)
        {
            state = unchecked(state * 1664525u + 1013904223u);
            return state;
        }

        static sbyte QuantizedAxis(uint value) =>
            (sbyte)((int)((value >> 24) % 255u) - 127);

        static bool IsFinite(AtlasMovementStateFrame state) =>
            IsFinite(state.PositionX) &&
            IsFinite(state.PositionY) &&
            IsFinite(state.PositionZ) &&
            IsFinite(state.VelocityX) &&
            IsFinite(state.VelocityY) &&
            IsFinite(state.VelocityZ) &&
            IsFinite(state.DirectionX) &&
            IsFinite(state.DirectionY) &&
            IsFinite(state.DirectionZ);

        static bool IsFinite(float value) =>
            !float.IsNaN(value) && !float.IsInfinity(value);
    }
}
