using System;
using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using System.Runtime.InteropServices;

namespace Atlas.Coro;

[AsyncMethodBuilder(typeof(AsyncAtlasTaskMethodBuilder))]
[StructLayout(LayoutKind.Auto)]
public readonly partial struct AtlasTask
{
    private readonly IAtlasTaskSource<AtlasUnit>? _source;
    private readonly Exception? _exception;
    private readonly short _token;

    internal AtlasTask(IAtlasTaskSource<AtlasUnit> source, short token)
    {
        _source = source;
        _exception = null;
        _token = token;
    }

    private AtlasTask(Exception ex)
    {
        _source = null;
        _exception = ex;
        _token = 0;
    }

    public static AtlasTask CompletedTask => default;

    public static AtlasTask FromException(Exception exception) =>
        new(exception ?? throw new ArgumentNullException(nameof(exception)));

    public static AtlasTask FromCanceled() =>
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
        private readonly AtlasTask _task;
        public Awaiter(AtlasTask task) => _task = task;

        public bool IsCompleted =>
            _task._source is null || _task._source.GetStatus(_task._token) != AtlasTaskStatus.Pending;

        public void GetResult()
        {
            if (_task._source is not null) { _task._source.GetResult(_task._token); return; }
            if (_task._exception is OperationCanceledException oce) throw oce;
            if (_task._exception is not null)
                ExceptionDispatchInfo.Capture(_task._exception).Throw();
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
