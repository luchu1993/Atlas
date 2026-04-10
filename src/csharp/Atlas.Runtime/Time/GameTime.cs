using Atlas.Core;

namespace Atlas;

/// <summary>
/// Provides engine timing values. Each access reads the latest value from C++.
/// </summary>
public static class Time
{
    /// <summary>Server wall-clock time in seconds since epoch.</summary>
    public static double ServerTime => NativeApi.ServerTime();

    /// <summary>Duration of the last frame in seconds.</summary>
    public static float DeltaTime => NativeApi.DeltaTime();
}
