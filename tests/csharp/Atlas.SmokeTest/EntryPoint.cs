using System.Runtime.InteropServices;
using System.Text;

namespace Atlas.SmokeTest;

public static class EntryPoint
{
    // Basic handshake: C++ calls this to verify CoreCLR is alive.
    [UnmanagedCallersOnly]
    public static int Ping()
    {
        return 42;
    }

    // Verify integer argument passing across the native boundary.
    [UnmanagedCallersOnly]
    public static int Add(int a, int b)
    {
        return a + b;
    }

    // Verify unsafe pointer interop: return the character length of a UTF-8 string.
    [UnmanagedCallersOnly]
    public static unsafe int StringLength(byte* utf8Ptr, int byteLen)
    {
        var span = new ReadOnlySpan<byte>(utf8Ptr, byteLen);
        return Encoding.UTF8.GetCharCount(span);
    }
}
