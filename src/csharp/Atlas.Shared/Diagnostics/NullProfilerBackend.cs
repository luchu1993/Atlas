using System;

namespace Atlas.Diagnostics
{
    /// <summary>
    /// Default backend used until a real one is installed. Every method is a
    /// no-op so a process that never calls <see cref="Profiler.SetBackend"/>
    /// pays only a single virtual dispatch per call site — and the JIT
    /// devirtualises that once <see cref="Profiler.Backend"/> is observed
    /// stable, which is the steady state in production.
    /// </summary>
    public sealed class NullProfilerBackend : IProfilerBackend
    {
        public static readonly NullProfilerBackend Instance = new NullProfilerBackend();

        private NullProfilerBackend()
        {
        }

        public IntPtr BeginZone(string name, uint color) => IntPtr.Zero;
        public void EndZone(IntPtr token) { }
        public void Plot(string name, double value) { }
        public void Message(string text) { }
        public void FrameMark(string name) { }
    }
}
