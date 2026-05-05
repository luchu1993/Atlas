using System;
using System.Threading;

namespace Atlas.Diagnostics;

// Layout: bits 32-63 unix_sec | bits 16-31 proc tag | bits 0-15 seq.
// Time-sortable across processes; 65536 ids/sec/proc cap.
public static class SnowflakeGen
{
    private static readonly ushort _procTag = ComputeProcTag();
    private static long _lastEpoch;
    private static int _seq;
    private static readonly object _lock = new();

    public static long Next()
    {
        long unixSec = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        ushort seq;
        lock (_lock)
        {
            if (unixSec != _lastEpoch) { _lastEpoch = unixSec; _seq = 0; }
            seq = (ushort)(_seq++ & 0xFFFF);
        }
        return (unixSec << 32) | ((long)_procTag << 16) | seq;
    }

    private static ushort ComputeProcTag()
    {
        int pid = System.Diagnostics.Process.GetCurrentProcess().Id;
        int nameHash = AppDomain.CurrentDomain.FriendlyName.GetHashCode();
        return (ushort)((pid ^ nameHash) & 0xFFFF);
    }
}
