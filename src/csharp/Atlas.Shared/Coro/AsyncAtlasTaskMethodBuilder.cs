using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Atlas.Coro;

[StructLayout(LayoutKind.Auto)]
public struct AsyncAtlasTaskMethodBuilder<T>
{
    private AtlasTaskSourceBoxBase<T>? _source;
    private short _token;
    private T _syncResult;
    private Exception? _syncException;
    private byte _syncState;       // 0 = pending / suspended via box, 1 = result, 2 = exception

    public static AsyncAtlasTaskMethodBuilder<T> Create() => default;

    public AtlasTask<T> Task
    {
        get
        {
            if (_source is not null) return new AtlasTask<T>(_source, _token);
            return _syncState switch
            {
                1 => new AtlasTask<T>(_syncResult),
                2 => AtlasTask<T>.FromException(_syncException!),
                _ => throw new InvalidOperationException("AtlasTask accessed before completion"),
            };
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Start<TStateMachine>(ref TStateMachine sm) where TStateMachine : IAsyncStateMachine
        => sm.MoveNext();

    public void SetResult(T result)
    {
        if (_source is null) { _syncResult = result; _syncState = 1; return; }
        _source.SetResult(result);
    }

    public void SetException(Exception exception)
    {
        if (_source is null) { _syncException = exception; _syncState = 2; return; }
        _source.SetException(exception);
    }

    public void AwaitOnCompleted<TAwaiter, TStateMachine>(ref TAwaiter awaiter, ref TStateMachine sm)
        where TAwaiter : INotifyCompletion
        where TStateMachine : IAsyncStateMachine
    {
        var box = EnsureBox(ref sm);
        awaiter.OnCompleted(box.MoveNextAction);
    }

    public void AwaitUnsafeOnCompleted<TAwaiter, TStateMachine>(ref TAwaiter awaiter, ref TStateMachine sm)
        where TAwaiter : ICriticalNotifyCompletion
        where TStateMachine : IAsyncStateMachine
    {
        var box = EnsureBox(ref sm);
        awaiter.UnsafeOnCompleted(box.MoveNextAction);
    }

    public void SetStateMachine(IAsyncStateMachine _) { }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private AtlasTaskSourceBox<TStateMachine, T> EnsureBox<TStateMachine>(ref TStateMachine sm)
        where TStateMachine : IAsyncStateMachine
    {
        if (_source is AtlasTaskSourceBox<TStateMachine, T> existing) return existing;
        var box = AtlasTaskSourceBox<TStateMachine, T>.Rent();
        // Store _source before copying the state machine so the boxed
        // copy picks up the same source reference.
        _token = box.Version;
        _source = box;
        box.SetStateMachine(ref sm);
        return box;
    }
}

[StructLayout(LayoutKind.Auto)]
public struct AsyncAtlasTaskMethodBuilder
{
    private AtlasTaskSourceBoxBase<AtlasUnit>? _source;
    private short _token;
    private Exception? _syncException;
    private byte _syncState;       // 0 = pending / suspended, 1 = success, 2 = exception

    public static AsyncAtlasTaskMethodBuilder Create() => default;

    public AtlasTask Task
    {
        get
        {
            if (_source is not null) return new AtlasTask(_source, _token);
            return _syncState switch
            {
                1 => AtlasTask.CompletedTask,
                2 => AtlasTask.FromException(_syncException!),
                _ => throw new InvalidOperationException("AtlasTask accessed before completion"),
            };
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Start<TStateMachine>(ref TStateMachine sm) where TStateMachine : IAsyncStateMachine
        => sm.MoveNext();

    public void SetResult()
    {
        if (_source is null) { _syncState = 1; return; }
        _source.SetResult(AtlasUnit.Default);
    }

    public void SetException(Exception exception)
    {
        if (_source is null) { _syncException = exception; _syncState = 2; return; }
        _source.SetException(exception);
    }

    public void AwaitOnCompleted<TAwaiter, TStateMachine>(ref TAwaiter awaiter, ref TStateMachine sm)
        where TAwaiter : INotifyCompletion
        where TStateMachine : IAsyncStateMachine
    {
        var box = EnsureBox(ref sm);
        awaiter.OnCompleted(box.MoveNextAction);
    }

    public void AwaitUnsafeOnCompleted<TAwaiter, TStateMachine>(ref TAwaiter awaiter, ref TStateMachine sm)
        where TAwaiter : ICriticalNotifyCompletion
        where TStateMachine : IAsyncStateMachine
    {
        var box = EnsureBox(ref sm);
        awaiter.UnsafeOnCompleted(box.MoveNextAction);
    }

    public void SetStateMachine(IAsyncStateMachine _) { }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private AtlasTaskSourceBox<TStateMachine, AtlasUnit> EnsureBox<TStateMachine>(ref TStateMachine sm)
        where TStateMachine : IAsyncStateMachine
    {
        if (_source is AtlasTaskSourceBox<TStateMachine, AtlasUnit> existing) return existing;
        var box = AtlasTaskSourceBox<TStateMachine, AtlasUnit>.Rent();
        _token = box.Version;
        _source = box;
        box.SetStateMachine(ref sm);
        return box;
    }
}
