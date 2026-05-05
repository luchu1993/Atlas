using System.Diagnostics;
using System.Globalization;

namespace Atlas.Client;

public interface IClientLogger
{
    void Info(string message);
    void Warn(string message);
    void Error(string message);
}

// Lines are prefixed with [t=S.sss] (monotonic seconds since first touch); world_stress tap strips it.
public static class ClientLog
{
    private sealed class NullLogger : IClientLogger
    {
        public void Info(string message) { }
        public void Warn(string message) { }
        public void Error(string message) { }
    }

    private static IClientLogger s_logger = new NullLogger();

    public static void SetLogger(IClientLogger? logger)
        => s_logger = logger ?? new NullLogger();

    private static readonly long s_originTicks = Stopwatch.GetTimestamp();
    private static readonly double s_secondsPerTick = 1.0 / Stopwatch.Frequency;

    public static string Timestamp()
    {
        var elapsed = (Stopwatch.GetTimestamp() - s_originTicks) * s_secondsPerTick;
        return "[t=" + elapsed.ToString("F3", CultureInfo.InvariantCulture) + "]";
    }

    public static void Info(string message)  => s_logger.Info(Timestamp() + " " + message);
    public static void Warn(string message)  => s_logger.Warn(Timestamp() + " " + message);
    public static void Error(string message) => s_logger.Error(Timestamp() + " " + message);
}
