using System;
using System.Threading;

namespace Atlas.Diagnostics;

// Layout: bits 32-63 unix_sec | bits 16-31 proc tag | bits 0-15 seq.
// Time-sortable across processes; 65536 ids/sec/proc cap.
public static class SnowflakeGen
{
    private static readonly ushort s_procTag = ComputeProcTag();
    private static long s_lastEpoch;
    private static int s_seq;
    private static readonly object s_lock = new();

    public static long Next()
    {
        long unixSec = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        ushort seq;
        lock (s_lock)
        {
            if (unixSec != s_lastEpoch) { s_lastEpoch = unixSec; s_seq = 0; }
            seq = (ushort)(s_seq++ & 0xFFFF);
        }
        return (unixSec << 32) | ((long)s_procTag << 16) | seq;
    }

    private static ushort ComputeProcTag()
    {
        int pid = System.Diagnostics.Process.GetCurrentProcess().Id;
        int nameHash = AppDomain.CurrentDomain.FriendlyName.GetHashCode();
        return (ushort)((pid ^ nameHash) & 0xFFFF);
    }
}
