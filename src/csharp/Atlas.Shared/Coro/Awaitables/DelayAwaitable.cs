using System;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Atlas.Coro;

// One-shot timer awaitable, pooled per-thread.
public sealed class DelayAwaitable
{
    private long _timerHandle;
    private CancelRegistration _cancelReg;
    private AtlasCancellationToken _ct;
    private Action? _continuation;
    private Exception? _exception;
    private int _completed;

    private static readonly Action<object?> OnTimerFired =
        static o => ((DelayAwaitable)o!).Complete(null);
    private static readonly Action<object?> OnCancelTriggered =
        static o => ((DelayAwaitable)o!).Complete(new OperationCanceledException());

    internal static DelayAwaitable Create(int milliseconds, AtlasCancellationToken ct)
    {
        if (milliseconds < 0) throw new ArgumentOutOfRangeException(nameof(milliseconds));

        var awaitable = TaskPool<DelayAwaitable>.TryRent(out var rented)
            ? rented!
            : new DelayAwaitable();

        awaitable._ct = ct;
        if (ct.IsCancellationRequested)
        {
            awaitable._exception = new OperationCanceledException();
            awaitable._completed = 1;
            return awaitable;
        }

        awaitable._timerHandle = AtlasLoop.Current.RegisterTimer(milliseconds, OnTimerFired, awaitable);
        if (ct.CanBeCanceled)
            awaitable._cancelReg = ct.Register(OnCancelTriggered, awaitable);
        return awaitable;
    }

    public Awaiter GetAwaiter() => new(this);

    private void Complete(Exception? exception)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0) return;
        if (exception is not null)
        {
            _exception = exception;
            AtlasLoop.Current.CancelTimer(_timerHandle);
        }
        _cancelReg.Dispose();

        var c = _continuation;
        _continuation = null;
        c?.Invoke();
    }

    private void Reset()
    {
        _timerHandle = 0;
        _cancelReg = default;
        _ct = default;
        _continuation = null;
        _exception = null;
        _completed = 0;
        TaskPool<DelayAwaitable>.Return(this);
    }

    public readonly struct Awaiter : ICriticalNotifyCompletion
    {
        private readonly DelayAwaitable _parent;
        public Awaiter(DelayAwaitable parent) => _parent = parent;

        public bool IsCompleted => Volatile.Read(ref _parent._completed) != 0;

        public void GetResult()
        {
            var ex = _parent._exception;
            _parent.Reset();
            if (ex is OperationCanceledException oce) throw oce;
            if (ex is not null) throw ex;
        }

        public void OnCompleted(Action continuation) => UnsafeOnCompleted(continuation);

        public void UnsafeOnCompleted(Action continuation)
        {
            if (continuation is null) throw new ArgumentNullException(nameof(continuation));
            if (Volatile.Read(ref _parent._completed) != 0) { continuation(); return; }
            _parent._continuation = continuation;
            // Re-check for a completion that raced past IsCompleted.
            if (Volatile.Read(ref _parent._completed) != 0)
            {
                var c = Interlocked.Exchange(ref _parent._continuation, null);
                c?.Invoke();
            }
        }
    }
}

public readonly partial struct AtlasTask
{
    public static DelayAwaitable Delay(int milliseconds, AtlasCancellationToken ct = default)
        => DelayAwaitable.Create(milliseconds, ct);
}
