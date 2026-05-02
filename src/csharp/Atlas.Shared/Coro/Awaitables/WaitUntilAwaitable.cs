using System;
using System.Runtime.CompilerServices;
using System.Threading;

namespace Atlas.Coro;

// Polls predicate once per frame; TState overload avoids closure allocation.
public sealed class WaitUntilAwaitable
{
    private Func<object?, bool>? _predicate;
    private object? _state;
    private CancelRegistration _cancelReg;
    private Action? _continuation;
    private Exception? _exception;
    private int _completed;

    private static readonly Action<object?> OnNextFrame =
        static o => ((WaitUntilAwaitable)o!).Poll();
    private static readonly Action<object?> OnCancel =
        static o => ((WaitUntilAwaitable)o!).Complete(new OperationCanceledException());

    internal static WaitUntilAwaitable Create(Func<object?, bool> predicate, object? state,
        AtlasCancellationToken ct)
    {
        if (predicate is null) throw new ArgumentNullException(nameof(predicate));

        var awaitable = TaskPool<WaitUntilAwaitable>.TryRent(out var rented)
            ? rented!
            : new WaitUntilAwaitable();

        awaitable._predicate = predicate;
        awaitable._state = state;

        if (ct.IsCancellationRequested)
        {
            awaitable._exception = new OperationCanceledException();
            awaitable._completed = 1;
            return awaitable;
        }

        if (predicate(state))
        {
            awaitable._completed = 1;
            return awaitable;
        }

        if (ct.CanBeCanceled)
            awaitable._cancelReg = ct.Register(OnCancel, awaitable);
        AtlasLoop.Current.PostNextFrame(OnNextFrame, awaitable);
        return awaitable;
    }

    public Awaiter GetAwaiter() => new(this);

    private void Poll()
    {
        if (Volatile.Read(ref _completed) != 0) return;
        bool ready;
        try { ready = _predicate!(_state); }
        catch (Exception ex) { Complete(ex); return; }
        if (ready) { Complete(null); return; }
        AtlasLoop.Current.PostNextFrame(OnNextFrame, this);
    }

    private void Complete(Exception? exception)
    {
        if (Interlocked.Exchange(ref _completed, 1) != 0) return;
        _exception = exception;
        _cancelReg.Dispose();

        var c = _continuation;
        _continuation = null;
        c?.Invoke();
    }

    private void Reset()
    {
        _predicate = null;
        _state = null;
        _cancelReg = default;
        _continuation = null;
        _exception = null;
        _completed = 0;
        TaskPool<WaitUntilAwaitable>.Return(this);
    }

    public readonly struct Awaiter : ICriticalNotifyCompletion
    {
        private readonly WaitUntilAwaitable _parent;
        public Awaiter(WaitUntilAwaitable parent) => _parent = parent;

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
    public static WaitUntilAwaitable WaitUntil(Func<bool> predicate,
        AtlasCancellationToken ct = default)
    {
        if (predicate is null) throw new ArgumentNullException(nameof(predicate));
        return WaitUntilAwaitable.Create(static p => ((Func<bool>)p!)(), predicate, ct);
    }

    public static WaitUntilAwaitable WaitUntil<TState>(Func<TState, bool> predicate, TState state,
        AtlasCancellationToken ct = default)
    {
        if (predicate is null) throw new ArgumentNullException(nameof(predicate));
        var pair = new StatePredicatePair<TState> { Predicate = predicate, State = state };
        return WaitUntilAwaitable.Create(StatePredicatePair<TState>.Invoke, pair, ct);
    }

    private sealed class StatePredicatePair<TState>
    {
        public Func<TState, bool> Predicate = null!;
        public TState State = default!;

        public static readonly Func<object?, bool> Invoke = static o =>
        {
            var p = (StatePredicatePair<TState>)o!;
            return p.Predicate(p.State);
        };
    }
}
