using System;
using System.Runtime.CompilerServices;

namespace Atlas.Diagnostics;

public static class Profiler
{
    private static IProfilerBackend _backend = NullProfilerBackend.Instance;

    public static IProfilerBackend Backend
    {
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        get => _backend;
    }

    // Reject installing a second non-null backend so Tracy and Unity Profiler
    // don't fight over the same call sites; idempotent for the same instance.
    public static bool SetBackend(IProfilerBackend backend)
    {
        if (backend == null) throw new ArgumentNullException(nameof(backend));
        var current = _backend;
        if (!ReferenceEquals(current, NullProfilerBackend.Instance) &&
            !ReferenceEquals(current, backend))
        {
            return false;
        }
        _backend = backend;
        return true;
    }

    public static void ResetBackend() => _backend = NullProfilerBackend.Instance;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ProfilerZone Zone([CallerMemberName] string name = "", uint color = 0)
    {
        var token = _backend.BeginZone(name, color);
        return new ProfilerZone(_backend, token);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static ProfilerZone ZoneN(string name, uint color = 0)
    {
        var token = _backend.BeginZone(name, color);
        return new ProfilerZone(_backend, token);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Plot(string name, double value) => _backend.Plot(name, value);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Plot(string name, int value) => _backend.Plot(name, value);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void Message(string text) => _backend.Message(text);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static void FrameMark(string name) => _backend.FrameMark(name);
}

public readonly struct ProfilerZone : IDisposable
{
    private readonly IProfilerBackend _backend;
    private readonly IntPtr _token;

    internal ProfilerZone(IProfilerBackend backend, IntPtr token)
    {
        _backend = backend;
        _token = token;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Dispose() => _backend?.EndZone(_token);
}
