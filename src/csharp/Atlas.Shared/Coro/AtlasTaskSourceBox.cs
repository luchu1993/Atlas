using System;
using System.Runtime.CompilerServices;

namespace Atlas.Coro;

// The non-generic base lets the builder hold the box without leaking TStateMachine.
internal abstract class AtlasTaskSourceBoxBase<T> : IAtlasTaskSource<T>
{
    protected AtlasTaskCompletionSourceCore<T> Core;

    public short Version => Core.Version;

    public AtlasTaskStatus GetStatus(short token) => Core.GetStatus(token);
    public void OnCompleted(Action<object?> cont, object? state, short token)
        => Core.OnCompleted(cont, state, token);

    public T GetResult(short token)
    {
        try { return Core.GetResult(token); }
        finally { OnConsumed(); }
    }

    public void SetResult(T result) => Core.TrySetResult(result);
    public void SetException(Exception exception) => Core.TrySetException(exception);

    protected abstract void OnConsumed();
}

internal sealed class AtlasTaskSourceBox<TStateMachine, T> : AtlasTaskSourceBoxBase<T>
    where TStateMachine : IAsyncStateMachine
{
    private TStateMachine _stateMachine = default!;
    private Action? _moveNextCached;

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static AtlasTaskSourceBox<TStateMachine, T> Rent()
    {
        if (TaskPool<AtlasTaskSourceBox<TStateMachine, T>>.TryRent(out var box))
            return box!;
        return new AtlasTaskSourceBox<TStateMachine, T>();
    }

    public Action MoveNextAction => _moveNextCached ??= MoveNext;

    public void SetStateMachine(ref TStateMachine sm) => _stateMachine = sm;

    private void MoveNext() => _stateMachine.MoveNext();

    protected override void OnConsumed()
    {
        Core.Reset();
        _stateMachine = default!;
        TaskPool<AtlasTaskSourceBox<TStateMachine, T>>.Return(this);
    }
}
