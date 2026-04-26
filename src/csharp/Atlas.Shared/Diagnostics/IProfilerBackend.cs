using System;

namespace Atlas.Diagnostics
{
    /// <summary>
    /// Pluggable backend that the static <see cref="Profiler"/> facade routes
    /// to. The token returned by <see cref="BeginZone"/> is opaque — backends
    /// own its meaning (Tracy uses the zone context pointer; Unity wraps a
    /// ProfilerMarker.AutoScope; the null backend ignores it). Returning
    /// <c>IntPtr.Zero</c> means "no zone was opened"; the matching
    /// <see cref="EndZone"/> call must still be invoked but is a no-op.
    /// </summary>
    /// <remarks>
    /// The interface is deliberately framework-neutral so Atlas.Shared can
    /// stay netstandard2.1 with no UnityEngine, no Tracy, and no other
    /// host-specific dependency. The desktop server installs the Tracy
    /// backend from Atlas.Runtime; the Unity client installs the
    /// ProfilerMarker backend from a Unity-only assembly. Trying to install
    /// two backends in one process is a programming error and
    /// <see cref="Profiler.SetBackend"/> rejects it with a warning.
    /// </remarks>
    public interface IProfilerBackend
    {
        IntPtr BeginZone(string name, uint color);
        void EndZone(IntPtr token);
        void Plot(string name, double value);
        void Message(string text);
        void FrameMark(string name);
    }
}
