using System;
using System.Text;
using Atlas.Core;

namespace Atlas.Diagnostics;

public sealed class NativeLogBackend : ILogBackend
{
    // 340 chars * 3 bytes/char (worst-case UTF-8) = 1020, fits in 1024.
    private const int kStackAllocCharLimit = 340;
    private const int kStackAllocByteSize = 1024;

    public void Log(int level, string message)
    {
        long traceId = TraceContext.Current;
        if (traceId != 0) message = $"[trace={traceId:X16}] {message}";
        if (message.Length <= kStackAllocCharLimit)
        {
            Span<byte> buf = stackalloc byte[kStackAllocByteSize];
            int written = Encoding.UTF8.GetBytes(message, buf);
            NativeApi.LogMessage(level, buf[..written]);
        }
        else
        {
            byte[] bytes = Encoding.UTF8.GetBytes(message);
            NativeApi.LogMessage(level, bytes);
        }
    }
}
