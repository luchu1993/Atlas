namespace Atlas.Coro;

// Fires once on host EngineShutdown — long-running awaits should pass
// AtlasShutdownToken.Token alongside their own cancellation token.
public static class AtlasShutdownToken
{
    private static AtlasCancellationSource? _source;

    public static AtlasCancellationToken Token =>
        _source is null ? AtlasCancellationToken.None : _source.Token;

    public static bool IsShutdownRequested =>
        _source is not null && _source.IsCancellationRequested;

    public static void Reset()
    {
        _source?.Dispose();
        _source = new AtlasCancellationSource();
    }

    public static void RequestShutdown() => _source?.Cancel();
}
