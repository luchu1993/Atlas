using System;

namespace Atlas.Coro;

// Backing source for AtlasTask<T>. Mirrors IValueTaskSource<T> from BCL but tied
// to AtlasTaskStatus and the Atlas single-threaded model (no completion flags).
public interface IAtlasTaskSource<out T>
{
    AtlasTaskStatus GetStatus(short token);
    void OnCompleted(Action<object?> continuation, object? state, short token);
    T GetResult(short token);
    short Version { get; }
}
