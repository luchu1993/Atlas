using System;

namespace Atlas.Coro;

public static class AtlasLoop
{
    private static IAtlasLoop _current = NullLoop.Instance;

    public static IAtlasLoop Current => _current;

    // Install the platform loop. Idempotent: same instance reinstalled is a no-op.
    public static void Install(IAtlasLoop loop)
    {
        if (loop is null) throw new ArgumentNullException(nameof(loop));
        _current = loop;
    }

    // Reset to NullLoop. Used by tests; production code should not call this.
    public static void Reset() => _current = NullLoop.Instance;
}

internal sealed class NullLoop : IAtlasLoop
{
    public static readonly NullLoop Instance = new();
    private NullLoop() { }

    public void PostNextFrame(Action<object?> cb, object? state)
        => throw new InvalidOperationException("AtlasLoop has not been installed");

    public long RegisterTimer(int milliseconds, Action<object?> cb, object? state)
        => throw new InvalidOperationException("AtlasLoop has not been installed");

    public void CancelTimer(long handle) { /* tolerate cancel after reset */ }

    public void PostMainThread(Action<object?> cb, object? state)
        => throw new InvalidOperationException("AtlasLoop has not been installed");

    public bool IsMainThread => true;
    public long CurrentFrame => 0;
}
