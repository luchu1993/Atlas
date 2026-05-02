using System;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;

namespace Atlas.Coro;

[AsyncMethodBuilder(typeof(AsyncAtlasTaskMethodBuilder<>))]
[StructLayout(LayoutKind.Auto)]
public readonly partial struct AtlasTask<T>
{
    private readonly IAtlasTaskSource<T>? _source;
    private readonly T _result;
    private readonly Exception? _exception;
    private readonly short _token;

    public AtlasTask(T result)
    {
        _source = null;
        _result = result;
        _exception = null;
        _token = 0;
    }

    internal AtlasTask(IAtlasTaskSource<T> source, short token)
    {
        _source = source;
        _result = default!;
        _exception = null;
        _token = token;
    }

    private AtlasTask(Exception exception)
    {
        _source = null;
        _result = default!;
        _exception = exception;
        _token = 0;
    }

    public static AtlasTask<T> FromResult(T value) => new(value);

    public static AtlasTask<T> FromException(Exception exception) =>
        new(exception ?? throw new ArgumentNullException(nameof(exception)));

    public static AtlasTask<T> FromCanceled() =>
        new(new OperationCanceledException("Operation canceled"));

    public AtlasTaskStatus Status =>
        _source is not null ? _source.GetStatus(_token)
        : _exception is OperationCanceledException ? AtlasTaskStatus.Canceled
        : _exception is not null ? AtlasTaskStatus.Faulted
        : AtlasTaskStatus.Succeeded;

    public Awaiter GetAwaiter() => new(this);

    [StructLayout(LayoutKind.Auto)]
    public readonly struct Awaiter : ICriticalNotifyCompletion
    {
        private readonly AtlasTask<T> _task;
        public Awaiter(AtlasTask<T> task) => _task = task;

        public bool IsCompleted =>
            _task._source is null || _task._source.GetStatus(_task._token) != AtlasTaskStatus.Pending;

        public T GetResult()
        {
            if (_task._source is not null) return _task._source.GetResult(_task._token);
            if (_task._exception is OperationCanceledException oce) throw oce;
            if (_task._exception is not null)
                ExceptionDispatchInfo.Capture(_task._exception).Throw();
            return _task._result;
        }

        public void OnCompleted(Action continuation) => UnsafeOnCompleted(continuation);

        public void UnsafeOnCompleted(Action continuation)
        {
            if (_task._source is null) { continuation(); return; }
            _task._source.OnCompleted(InvokeAction, continuation, _task._token);
        }

        private static readonly Action<object?> InvokeAction = static o => ((Action)o!)();
    }
}
