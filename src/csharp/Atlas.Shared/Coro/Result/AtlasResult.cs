using System;

namespace Atlas.Coro;

public readonly struct AtlasResult<T, TError>
{
    private readonly T _value;
    private readonly TError _error;
    private readonly bool _isOk;

    private AtlasResult(T value, TError error, bool isOk)
    {
        _value = value;
        _error = error;
        _isOk = isOk;
    }

    public static AtlasResult<T, TError> Ok(T value) => new(value, default!, true);
    public static AtlasResult<T, TError> Err(TError error) => new(default!, error, false);

    public bool IsOk => _isOk;
    public bool IsError => !_isOk;

    public T Value => _isOk
        ? _value
        : throw new InvalidOperationException("AtlasResult: accessing Value on error variant");

    public TError Error => !_isOk
        ? _error
        : throw new InvalidOperationException("AtlasResult: accessing Error on ok variant");

    public bool TryGetValue(out T value)
    {
        value = _value;
        return _isOk;
    }

    public bool TryGetError(out TError error)
    {
        error = _error;
        return !_isOk;
    }

    public static implicit operator AtlasResult<T, TError>(T value) => Ok(value);
    public static implicit operator AtlasResult<T, TError>(TError error) => Err(error);
}
