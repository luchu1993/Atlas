using System;
using System.Diagnostics;
using System.Globalization;
using Atlas.Diagnostics;

namespace Atlas.Client.Desktop;

// Trace/Debug/Info -> stdout, Warning/Error/Critical -> stderr.
// world_stress's stdout tap relies on the [t=...] prefix (see test_client_event_tap.cpp).
public sealed class ConsoleLogBackend : ILogBackend
{
    private static readonly long s_originTicks = Stopwatch.GetTimestamp();
    private static readonly double s_secondsPerTick = 1.0 / Stopwatch.Frequency;

    public void Log(int level, string message)
    {
        var elapsed = (Stopwatch.GetTimestamp() - s_originTicks) * s_secondsPerTick;
        var line = "[t=" + elapsed.ToString("F3", CultureInfo.InvariantCulture) + "] " + message;
        if (level <= 2) Console.WriteLine(line);
        else Console.Error.WriteLine(line);
    }
}
