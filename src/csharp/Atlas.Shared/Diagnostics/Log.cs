using System;
using System.Runtime.CompilerServices;

namespace Atlas.Diagnostics;

public static class Log
{
    private static ILogBackend _backend = NullLogBackend.Instance;

    public static ILogBackend Backend
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => _backend;
    }

    // Reject re-install with a different backend so a misordered bootstrap
    // surfaces instead of one host silently winning over another.
    public static bool SetBackend(ILogBackend backend)
    {
        if (backend == null) throw new ArgumentNullException(nameof(backend));
        var current = _backend;
        if (!ReferenceEquals(current, NullLogBackend.Instance) &&
            !ReferenceEquals(current, backend))
        {
            return false;
        }
        _backend = backend;
        return true;
    }

    public static void ResetBackend() => _backend = NullLogBackend.Instance;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Trace(string message) => _backend.Log(0, message);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Debug(string message) => _backend.Log(1, message);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Info(string message) => _backend.Log(2, message);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Warning(string message) => _backend.Log(3, message);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Error(string message) => _backend.Log(4, message);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Critical(string message) => _backend.Log(5, message);
}
