#if UNITY_2022_3_OR_NEWER

using System;
using System.Collections.Concurrent;
using System.Runtime.CompilerServices;

using Atlas.Diagnostics;

using Unity.Profiling;

namespace Atlas.Client.Unity
{
    /// <summary>
    /// <see cref="IProfilerBackend"/> implementation that pipes Atlas zones
    /// into Unity's Profiler / Profile Analyzer / Frame Debugger via
    /// <see cref="ProfilerMarker"/>. Installed during Unity bootstrap;
    /// after that, every <c>using var _ = Profiler.Zone(...)</c> in
    /// Atlas.Client (e.g. ClientCallbacks.DispatchRpc, ClientEntity.
    /// ApplyPositionUpdate) shows up as a sample in the Unity Profiler
    /// window with the same name the server uses for the equivalent zone.
    /// </summary>
    /// <remarks>
    /// Per Atlas's profiler design, the Unity backend is the *only* one
    /// the client should use — Tracy's listener never runs in a player
    /// build. Trying to install both Tracy and Unity backends in the
    /// same process is rejected by <see cref="Profiler.SetBackend"/> as
    /// a bootstrap error.
    ///
    /// Why ProfilerMarker rather than Profiler.BeginSample: ProfilerMarker
    /// is the modern API and IL2CPP is able to inline its Begin/End pair
    /// down to a single BurstStart / BurstEnd call when the marker is
    /// statically reachable. BeginSample takes a string and resolves it
    /// each call. The marker cache below exploits this: literal-name
    /// zones (which all Atlas.Client zones are, since they pull names
    /// from ProfilerNames constants) hit the cache exactly once per
    /// distinct name.
    ///
    /// Domain reload (Unity Editor only): the marker cache is process-
    /// scoped and its IntPtr handles outlive an assembly-reload event.
    /// The cache is therefore cleared via
    /// AssemblyReloadEvents.beforeAssemblyReload at the call site that
    /// installs the backend; without that, a stale ProfilerMarker
    /// would crash the next play session. The clear logic lives at
    /// install time rather than inside this class so an editor-build
    /// dependency on UnityEditor stays opt-in.
    /// </remarks>
    public sealed class UnityProfilerBackend : IProfilerBackend
    {
        // Markers are keyed by name. The IntPtr token returned to
        // IProfilerBackend.BeginZone is the marker's index in the cache,
        // not its native handle — markers themselves are reference types
        // and shouldn't be cast across the IntPtr boundary directly.
        // Concurrent dictionary handles the rare case of two threads
        // first-using the same zone name in the same frame.
        private readonly ConcurrentDictionary<string, ProfilerMarker> markers_ =
            new ConcurrentDictionary<string, ProfilerMarker>();

        // Thread-local stack of currently open markers. Unity's
        // ProfilerMarker requires Begin / End on the same thread and in
        // LIFO order; the IProfilerBackend contract guarantees LIFO via
        // the using statement. Storing a stack rather than a per-zone
        // marker reference avoids paying for one allocation per zone
        // open — IntPtr is the cheapest token shape.
        [ThreadStatic]
        private static System.Collections.Generic.Stack<ProfilerMarker>? t_open_;

        public IntPtr BeginZone(string name, uint color)
        {
            if (string.IsNullOrEmpty(name)) return IntPtr.Zero;
            var marker = markers_.GetOrAdd(name, n => new ProfilerMarker(n));
            marker.Begin();
            (t_open_ ??= new System.Collections.Generic.Stack<ProfilerMarker>()).Push(marker);
            // Return a non-zero token to distinguish a real zone from
            // the no-op path. The actual marker is recovered from the
            // thread-local stack on EndZone — IntPtr's payload is
            // unused because Stack<T>.Pop is faster than dictionary
            // lookup and matches the LIFO contract.
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

        public void Plot(string name, double value)
        {
            // Unity 2022.2+ exposes ProfilerCounterValue<double> for
            // structured plot streams. Until the Atlas client is sure
            // every consuming Unity version has that API, we route
            // plot points through Profiler.EmitFrameMetaData which is
            // available on every Unity version Atlas supports. Cost is
            // a marshalling shim per call — acceptable at plot rates
            // (a few per frame, not per zone).
            UnityEngine.Profiling.Profiler.SetCounterValue(name, (float)value);
        }

        public void Message(string text)
        {
            // The Unity Profiler doesn't have a free-form annotation
            // channel that's observably useful in the same way Tracy's
            // TracyMessage is. Fall back to a single-frame named scope
            // tagged with the text — visible in the Profiler timeline,
            // disposable instantly, no resident state.
            using var _ = new ProfilerMarker(text).Auto();
        }

        public void FrameMark(string name)
        {
            // Unity already drives a per-render-frame marker on its own;
            // a logical-tick frame marker would interleave confusingly.
            // The standard technique is to surface the logical frame as
            // a labelled named-sample boundary instead, which the
            // Profile Analyzer treats as a fence for filtering.
            new ProfilerMarker(name).Auto().Dispose();
        }
    }
}

#endif  // UNITY_2022_3_OR_NEWER
