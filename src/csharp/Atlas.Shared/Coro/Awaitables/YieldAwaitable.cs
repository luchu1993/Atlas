using System;
using System.Runtime.CompilerServices;

namespace Atlas.Coro;

public readonly struct YieldAwaitable
{
    public Awaiter GetAwaiter() => default;

    public readonly struct Awaiter : ICriticalNotifyCompletion
    {
        public bool IsCompleted => false;
        public void GetResult() { }

        public void OnCompleted(Action continuation) => UnsafeOnCompleted(continuation);

        public void UnsafeOnCompleted(Action continuation)
        {
            if (continuation is null) throw new ArgumentNullException(nameof(continuation));
            AtlasLoop.Current.PostNextFrame(InvokeAction, continuation);
        }

        private static readonly Action<object?> InvokeAction = static o => ((Action)o!)();
    }
}

public readonly partial struct AtlasTask
{
    public static YieldAwaitable Yield() => default;
}
