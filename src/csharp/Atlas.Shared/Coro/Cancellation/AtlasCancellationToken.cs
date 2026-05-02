using System;

namespace Atlas.Coro;

public readonly struct AtlasCancellationToken : IEquatable<AtlasCancellationToken>
{
    private readonly AtlasCancellationSource? _source;

    internal AtlasCancellationToken(AtlasCancellationSource source)
    {
        _source = source;
    }

    public static AtlasCancellationToken None => default;

    public bool IsCancellationRequested =>
        _source is not null && _source.IsCancellationRequested;

    public bool CanBeCanceled => _source is not null;

    public CancelRegistration Register(Action<object?> callback, object? state)
    {
        if (callback is null) throw new ArgumentNullException(nameof(callback));
        if (_source is null) return default;
        var id = _source.RegisterCallback(callback, state);
        return id == 0 ? default : new CancelRegistration(_source, id);
    }

    public void ThrowIfCancellationRequested()
    {
        if (IsCancellationRequested) throw new OperationCanceledException();
    }

    public bool Equals(AtlasCancellationToken other) => ReferenceEquals(_source, other._source);
    public override bool Equals(object? obj) => obj is AtlasCancellationToken o && Equals(o);
    public override int GetHashCode() => _source?.GetHashCode() ?? 0;
}
