using System;
using System.Runtime.InteropServices;

namespace Atlas.Runtime.Diagnostics
{
    /// <summary>
    /// Minimal P/Invoke bindings for Tracy 0.13.x's TracyC.h surface — just
    /// the entry points <see cref="TracyProfilerBackend"/> needs.
    /// Library name <c>TracyClient</c> matches the CMake target built as a
    /// SHARED library in cmake/Dependencies.cmake; both the C++ link path
    /// and these P/Invoke calls resolve to the same in-process Tracy
    /// instance, which is what merges native and managed zones into one
    /// trace timeline.
    /// </summary>
    /// <remarks>
    /// Source-generated <see cref="LibraryImportAttribute"/> stubs match
    /// Atlas's prevailing interop style (see Core/NativeApi.cs). All
    /// arguments here are blittable (primitives or unmanaged pointers) so
    /// the generator emits zero-marshalling fast paths — important on the
    /// zone open/close hot path.
    ///
    /// This file deliberately re-implements what packages like Tracy-NET
    /// would otherwise provide, because the available NuGet packages in
    /// April 2026 either pin the Tracy native version too low (Tracy-CSharp
    /// 0.11.1) or require net10.0 (Tracy-NET 0.13.x) — Atlas.Runtime is
    /// net9.0 and the native layer just bumped to Tracy 0.13.1, so neither
    /// fits. Ownership of the bindings keeps the version-pin policy
    /// trivial: bump the Tracy native tag in cmake/Dependencies.cmake,
    /// then re-check the function list below against the new TracyC.h.
    /// </remarks>
    internal static unsafe partial class TracyNative
    {
        private const string LibraryName = "TracyClient";

        /// <summary>
        /// Tracy's zone context. Layout matches TracyC.h's
        /// <c>___tracy_c_zone_context</c> exactly so the struct can be
        /// returned by value across the P/Invoke boundary and bit-cast to
        /// IntPtr at the IProfilerBackend layer (both are 8 bytes on x64).
        /// </summary>
        [StructLayout(LayoutKind.Sequential)]
        public struct TracyCZoneCtx
        {
            public uint Id;
            public int Active;
        }

        // Source-location allocation. Returns an opaque uint64 token that
        // ___tracy_emit_zone_begin_alloc consumes. Tracy copies the bytes
        // from the supplied buffers into its own arena, so the caller's
        // buffers can be freed immediately after the call returns.
        [LibraryImport(LibraryName, EntryPoint = "___tracy_alloc_srcloc_name")]
        public static partial ulong AllocSrclocName(
            uint line, byte* source, nuint sourceSz,
            byte* function, nuint functionSz,
            byte* name, nuint nameSz, uint color);

        [LibraryImport(LibraryName, EntryPoint = "___tracy_emit_zone_begin_alloc")]
        public static partial TracyCZoneCtx EmitZoneBeginAlloc(ulong srcloc, int active);

        [LibraryImport(LibraryName, EntryPoint = "___tracy_emit_zone_end")]
        public static partial void EmitZoneEnd(TracyCZoneCtx ctx);

        [LibraryImport(LibraryName, EntryPoint = "___tracy_emit_message")]
        public static partial void EmitMessage(byte* txt, nuint size, int callstackDepth);

        // Frame mark accepts a const char* whose pointer identity Tracy uses
        // to key the frame group — so the buffer must outlive the program,
        // not the call. The managed wrapper caches a marshalled copy per
        // distinct frame name and re-passes that same pointer on every tick.
        [LibraryImport(LibraryName, EntryPoint = "___tracy_emit_frame_mark")]
        public static partial void EmitFrameMark(IntPtr name);

        // Same pointer-identity convention as frame names — the plot label
        // pointer must be stable for the life of the process.
        [LibraryImport(LibraryName, EntryPoint = "___tracy_emit_plot")]
        public static partial void EmitPlot(IntPtr name, double val);

        [LibraryImport(LibraryName, EntryPoint = "___tracy_connected")]
        public static partial int Connected();

        [LibraryImport(LibraryName, EntryPoint = "___tracy_set_thread_name")]
        public static partial void SetThreadName(byte* name);
    }
}
