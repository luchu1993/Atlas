using System;
using System.Collections.Generic;

namespace Atlas.Client;

public interface IMovementCurveSampler
{
    bool Contains(ushort curveId);
    float Sample(ushort curveId, float normalizedTime);
    float TimeAtProgress(ushort curveId, float progress);
}

public sealed class MovementCurveRegistry : IMovementCurveSampler
{
    public const int MaxSamples = 64;

    readonly object _gate = new();
    volatile Dictionary<ushort, float[]> _curves = CreateDefaultCurves();

    public bool Register(ushort curveId, ReadOnlySpan<float> samples)
    {
        if (samples.IsEmpty || samples.Length > MaxSamples) return false;

        var copy = new float[samples.Length];
        for (int i = 0; i < samples.Length; ++i)
        {
            float sample = samples[i];
            if (float.IsNaN(sample) || float.IsInfinity(sample)) return false;
            copy[i] = sample;
        }

        lock (_gate)
        {
            var next = new Dictionary<ushort, float[]>(_curves) { [curveId] = copy };
            _curves = next;
        }
        return true;
    }

    public bool Contains(ushort curveId) => _curves.ContainsKey(curveId);

    public float Sample(ushort curveId, float normalizedTime)
    {
        if (float.IsNaN(normalizedTime) || float.IsInfinity(normalizedTime)) return 0.0f;
        if (!_curves.TryGetValue(curveId, out var samples)) return SampleLinear(normalizedTime);
        return Sample(samples, normalizedTime);
    }

    public float TimeAtProgress(ushort curveId, float progress)
    {
        if (float.IsNaN(progress) || float.IsInfinity(progress)) return 0.0f;
        progress = Math.Clamp(progress, 0.0f, 1.0f);
        if (!_curves.TryGetValue(curveId, out var samples)) return progress;
        if (samples.Length <= 1) return progress;

        float bestTime = 0.0f;
        float bestError = MathF.Abs(samples[0] - progress);
        for (int i = 0; i + 1 < samples.Length; ++i)
        {
            float a = samples[i];
            float b = samples[i + 1];
            float segmentMin = MathF.Min(a, b);
            float segmentMax = MathF.Max(a, b);
            if (progress >= segmentMin && progress <= segmentMax && MathF.Abs(b - a) > 0.0001f)
            {
                float fraction = (progress - a) / (b - a);
                return (i + fraction) / (samples.Length - 1);
            }

            float error = MathF.Abs(b - progress);
            if (error < bestError)
            {
                bestError = error;
                bestTime = (i + 1.0f) / (samples.Length - 1);
            }
        }
        return Math.Clamp(bestTime, 0.0f, 1.0f);
    }

    static float Sample(float[] samples, float normalizedTime)
    {
        if (samples.Length == 1) return samples[0];

        float time = Math.Clamp(normalizedTime, 0.0f, 1.0f);
        float scaled = time * (samples.Length - 1);
        int index = (int)MathF.Floor(scaled);
        if (index + 1 >= samples.Length) return samples[^1];

        float fraction = scaled - index;
        return samples[index] * (1.0f - fraction) + samples[index + 1] * fraction;
    }

    static float SampleLinear(float normalizedTime) => Math.Clamp(normalizedTime, 0.0f, 1.0f);

    static Dictionary<ushort, float[]> CreateDefaultCurves() =>
        new() { [0] = new[] { 0.0f, 1.0f } };
}

public static class MovementCurves
{
    public static MovementCurveRegistry Default { get; } = new();

    public static bool Register(ushort curveId, ReadOnlySpan<float> samples) =>
        Default.Register(curveId, samples);

    public static bool Contains(ushort curveId) => Default.Contains(curveId);

    public static float Sample(ushort curveId, float normalizedTime) =>
        Default.Sample(curveId, normalizedTime);

    internal static float TimeAtProgress(ushort curveId, float progress) =>
        Default.TimeAtProgress(curveId, progress);
}
