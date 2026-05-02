using System;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Threading;

namespace Atlas.Coro;

// completedCount gates exactly-once Pending→Completed; version bumps on Reset
// for ABA defence when the source is pooled.
public struct AtlasTaskCompletionSourceCore<T>
{
    private T _result;
    private object? _error;
    private short _version;
    private int _completedCount;
    private Action<object?>? _continuation;
    private object? _continuationState;

    public short Version => _version;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Reset()
    {
        unchecked { _version++; }
        _completedCount = 0;
        _result = default!;
        _error = null;
        _continuation = null;
        _continuationState = null;
    }

    public bool TrySetResult(T result)
    {
        if (Interlocked.Increment(ref _completedCount) != 1) return false;
        _result = result;
        SignalCompletion();
        return true;
    }

    public bool TrySetException(Exception exception)
    {
        if (exception is null) throw new ArgumentNullException(nameof(exception));
        if (Interlocked.Increment(ref _completedCount) != 1) return false;
        _error = exception is OperationCanceledException
            ? exception
            : ExceptionDispatchInfo.Capture(exception);
        SignalCompletion();
        return true;
    }

    public bool TrySetCanceled()
    {
        if (Interlocked.Increment(ref _completedCount) != 1) return false;
        _error = CachedOperationCanceled.Instance;
        SignalCompletion();
        return true;
    }

    public AtlasTaskStatus GetStatus(short token)
    {
        ValidateToken(token);
        if (Volatile.Read(ref _completedCount) == 0) return AtlasTaskStatus.Pending;
        return _error switch
        {
            null                          => AtlasTaskStatus.Succeeded,
            OperationCanceledException    => AtlasTaskStatus.Canceled,
            _                             => AtlasTaskStatus.Faulted,
        };
    }

    public T GetResult(short token)
    {
        ValidateToken(token);
        var err = _error;
        var res = _result;
        if (err is null) return res;
        if (err is OperationCanceledException oce) throw oce;
        ((ExceptionDispatchInfo)err).Throw();
        return default!; // unreachable
    }

    public void OnCompleted(Action<object?> continuation, object? state, short token)
    {
        if (continuation is null) throw new ArgumentNullException(nameof(continuation));
        ValidateToken(token);

        // Pre-completed: run inline.
        if (Volatile.Read(ref _completedCount) != 0)
        {
            continuation(state);
            return;
        }

        _continuationState = state;
        var prev = Interlocked.CompareExchange(ref _continuation, continuation, null);
        if (prev is null) return;                             // SignalCompletion will fire it
        if (ReferenceEquals(prev, CompletedSentinel))         // completion raced ahead of CAS
        {
            continuation(state);
            return;
        }
        throw new InvalidOperationException(
            "AtlasTaskCompletionSourceCore: continuation already registered");
    }

    private void SignalCompletion()
    {
        var cont = Interlocked.Exchange(ref _continuation, CompletedSentinel);
        if (cont is null || ReferenceEquals(cont, CompletedSentinel)) return;
        cont(_continuationState);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void ValidateToken(short token)
    {
        if (token != _version)
            throw new InvalidOperationException(
                "AtlasTaskCompletionSourceCore: stale token (source was reused)");
    }

    private static readonly Action<object?> CompletedSentinel = static _ => { };

    private static class CachedOperationCanceled
    {
        public static readonly OperationCanceledException Instance =
            new("Operation canceled");
    }
}
