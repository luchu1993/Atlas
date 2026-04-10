using System;
using System.Text;
using Atlas.Core;

namespace Atlas;

/// <summary>
/// Engine logging facade. Forwards messages to the C++ logging system
/// via <see cref="NativeApi.LogMessage"/>.
/// </summary>
public static class Log
{
    public static void Trace(string message) => Send(0, message);
    public static void Debug(string message) => Send(1, message);
    public static void Info(string message) => Send(2, message);
    public static void Warning(string message) => Send(3, message);
    public static void Error(string message) => Send(4, message);
    public static void Critical(string message) => Send(5, message);

    // 340 chars * 3 bytes/char (worst-case UTF-8) = 1020, comfortably fits in 1024.
    private const int StackAllocCharLimit = 340;
    private const int StackAllocByteSize = 1024;

    private static void Send(int level, string message)
    {
        if (message.Length <= StackAllocCharLimit)
        {
            Span<byte> buf = stackalloc byte[StackAllocByteSize];
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
