using System;

namespace Atlas.Diagnostics;

// Token returned by BeginZone is opaque — backends own its meaning (Tracy
// uses zone context pointer; Unity wraps ProfilerMarker.AutoScope; null
// backend ignores it). IntPtr.Zero means "no zone opened" but EndZone is
// still invoked (no-op).
public interface IProfilerBackend
{
    IntPtr BeginZone(string name, uint color);
    void EndZone(IntPtr token);
    void Plot(string name, double value);
    void Message(string text);
    void FrameMark(string name);
}
