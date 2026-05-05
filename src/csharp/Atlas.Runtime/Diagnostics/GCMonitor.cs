using System;
using System.Threading;

namespace Atlas.Diagnostics;

internal static class GCMonitor
{
    private static Timer? _timer;

    public static void Start(TimeSpan interval)
    {
        Stop();
        _timer = new Timer(_ => Report(), null, interval, interval);
        Log.Info($"GCMonitor: started with interval {interval.TotalSeconds}s");
    }

    public static void Stop()
    {
        _timer?.Dispose();
        _timer = null;
    }

    public static void Report()
    {
        try
        {
            var info = GC.GetGCMemoryInfo();
            var heapMB = info.HeapSizeBytes / (1024.0 * 1024.0);
            Log.Info(
                $"GC: heap={heapMB:F1}MB " +
                $"gen0={GC.CollectionCount(0)} " +
                $"gen1={GC.CollectionCount(1)} " +
                $"gen2={GC.CollectionCount(2)} " +
                $"pause={info.PauseTimePercentage:F2}%");
        }
        catch (Exception ex)
        {
            Log.Warning($"GCMonitor: failed to report: {ex.Message}");
        }
    }
}
