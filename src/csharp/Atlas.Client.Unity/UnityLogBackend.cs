using System.Globalization;
using Atlas.Diagnostics;
using UnityEngine;

namespace Atlas.Client.Unity
{
    public sealed class UnityLogBackend : ILogBackend
    {
        private static readonly long s_originTicks = System.Diagnostics.Stopwatch.GetTimestamp();
        private static readonly double s_secondsPerTick = 1.0 / System.Diagnostics.Stopwatch.Frequency;

        public void Log(int level, string message)
        {
            var elapsed = (System.Diagnostics.Stopwatch.GetTimestamp() - s_originTicks)
                          * s_secondsPerTick;
            var line = "[t=" + elapsed.ToString("F3", CultureInfo.InvariantCulture) + "] "
                       + message;
            if (level <= 2) Debug.Log(line);
            else if (level == 3) Debug.LogWarning(line);
            else Debug.LogError(line);
        }
    }
}
