using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Runtime.CompilerServices;

using Atlas.Diagnostics;

using Unity.Profiling;

namespace Atlas.Client.Unity
{
    public sealed class UnityProfilerBackend : IProfilerBackend
    {
        private readonly ConcurrentDictionary<string, ProfilerMarker> markers_ = new();

        // ProfilerMarker.Begin/End must pair LIFO on the same thread.
        [ThreadStatic]
        private static Stack<ProfilerMarker>? t_open_;

        public IntPtr BeginZone(string name, uint color)
        {
            if (string.IsNullOrEmpty(name)) return IntPtr.Zero;
            var marker = markers_.GetOrAdd(name, static n => new ProfilerMarker(n));
            marker.Begin();
            (t_open_ ??= new Stack<ProfilerMarker>()).Push(marker);
            return new IntPtr(1);
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void EndZone(IntPtr token)
        {
            if (token == IntPtr.Zero) return;
            var stack = t_open_;
            if (stack == null || stack.Count == 0) return;
            stack.Pop().End();
        }

        // No engine-bundled name->value plot API; ProfilerCounterValue<T> ships in the
        // com.unity.profiling.core UPM package — wire it on the first real Plot caller.
        public void Plot(string name, double value) { }

        // Unity has no free-form annotation channel; degrade to a zero-duration named scope.
        public void Message(string text)
        {
            using var _ = new ProfilerMarker(text).Auto();
        }

        // Unity already drives its own render-frame marker; this names the logical
        // tick as a sample boundary that Profile Analyzer treats as a filter fence.
        public void FrameMark(string name)
        {
            using var _ = new ProfilerMarker(name).Auto();
        }
    }
}
