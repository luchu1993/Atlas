using System;
using System.Diagnostics;
using System.Globalization;

namespace Atlas.Client;

/// <summary>
/// Minimal stdout/stderr helper used by client-side scripts and the
/// ClientEntity base. Every line gets a <c>[t=S.sss]</c> prefix where
/// <c>S.sss</c> is seconds (monotonic, 3-decimal) since the first
/// touch of this class inside the process.
/// </summary>
/// <remarks>
/// The world_stress tap (<c>client_event_tap.cc</c>) strips the
/// timestamp before matching event tokens, so the prefix is
/// backward-compatible with any existing counter; downstream
/// convergence-analysis scripts can lift the stamp back out by
/// matching <c>^\[t=([0-9.]+)\] </c>.
/// <para/>
/// The timestamp origin is the moment ClientLog is first used, not
/// process start — close enough (milliseconds after ClR init) for
/// correlation with the cluster_control runner, and avoids extra
/// plumbing to hand the process start time around.
/// </remarks>
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
