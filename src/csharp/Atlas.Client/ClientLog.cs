using System;
using System.Diagnostics;
using System.Globalization;

namespace Atlas.Client;

// Lines are prefixed with [t=S.sss] (monotonic seconds since first touch); world_stress tap strips it.
public static class ClientLog
{
    private static readonly long s_originTicks = Stopwatch.GetTimestamp();
    private static readonly double s_secondsPerTick = 1.0 / Stopwatch.Frequency;

    public static string Timestamp()
    {
        var elapsed = (Stopwatch.GetTimestamp() - s_originTicks) * s_secondsPerTick;
        return "[t=" + elapsed.ToString("F3", CultureInfo.InvariantCulture) + "]";
    }

    public static void Info(string message)
    {
        Console.WriteLine(Timestamp() + " " + message);
    }

    public static void Warn(string message)
    {
        Console.Error.WriteLine(Timestamp() + " " + message);
    }
}
