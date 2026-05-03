using System;
using System.Threading;

namespace Atlas.Diagnostics;

public static class TraceContext
{
    private static readonly AsyncLocal<long> s_current = new();

    public static long Current => s_current.Value;

    public static Scope Push(long traceId) => new(traceId);

    // Mints a fresh id when the inbound RPC carried zero so business code
    // never sees Current == 0; non-zero passes through unchanged.
    public static Scope BeginInbound(long incoming)
        => new(incoming != 0 ? incoming : SnowflakeGen.Next());

    public readonly struct Scope : IDisposable
    {
        private readonly long _previous;
        internal Scope(long traceId)
        {
            _previous = s_current.Value;
            s_current.Value = traceId;
        }
        public void Dispose() => s_current.Value = _previous;
    }
}
