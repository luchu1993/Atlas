using System;

namespace Atlas.Coro;

public readonly struct CancelRegistration : IDisposable, IEquatable<CancelRegistration>
{
    private readonly AtlasCancellationSource? _source;
    private readonly long _id;

    internal CancelRegistration(AtlasCancellationSource source, long id)
    {
        _source = source;
        _id = id;
    }

    public void Dispose()
    {
        if (_source is not null && _id != 0)
            _source.UnregisterCallback(_id);
    }

    public bool Equals(CancelRegistration other) =>
        ReferenceEquals(_source, other._source) && _id == other._id;

    public override bool Equals(object? obj) => obj is CancelRegistration o && Equals(o);
    public override int GetHashCode() => HashCode.Combine(_source, _id);
}
