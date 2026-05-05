using System;

namespace Atlas.Diagnostics;

public sealed class NullProfilerBackend : IProfilerBackend
{
    public static readonly NullProfilerBackend Instance = new();

    private NullProfilerBackend() { }

    public IntPtr BeginZone(string name, uint color) => IntPtr.Zero;
    public void EndZone(IntPtr token) { }
    public void Plot(string name, double value) { }
    public void Message(string text) { }
    public void FrameMark(string name) { }
}
