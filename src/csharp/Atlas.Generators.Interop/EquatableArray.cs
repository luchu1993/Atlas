using System;
using System.Collections;
using System.Collections.Generic;
using System.Collections.Immutable;

namespace Atlas.Generators.Interop;

/// <summary>
/// Wraps <see cref="ImmutableArray{T}"/> with value-semantic equality
/// (via <see cref="SequenceEqual"/>).  Required for incremental generator
/// cache correctness — without this, the pipeline would re-run on every
/// compilation because <see cref="ImmutableArray{T}"/> uses reference equality.
/// </summary>
internal readonly struct EquatableArray<T> : IEquatable<EquatableArray<T>>, IEnumerable<T>
    where T : IEquatable<T>
{
    private readonly ImmutableArray<T> _array;

    public EquatableArray(ImmutableArray<T> array) => _array = array;

    public int Length => _array.IsDefault ? 0 : _array.Length;
    public T this[int index] => _array[index];

    public bool Equals(EquatableArray<T> other)
    {
        if (_array.IsDefault && other._array.IsDefault)
            return true;
        if (_array.IsDefault || other._array.IsDefault)
            return false;
        if (_array.Length != other._array.Length)
            return false;

        for (int i = 0; i < _array.Length; i++)
        {
            if (!_array[i].Equals(other._array[i]))
                return false;
        }

        return true;
    }

    public override bool Equals(object? obj) => obj is EquatableArray<T> other && Equals(other);

    public override int GetHashCode()
    {
        if (_array.IsDefault)
            return 0;

        int hash = 17;
        foreach (var item in _array)
            hash = hash * 31 + item.GetHashCode();
        return hash;
    }

    public ImmutableArray<T>.Enumerator GetEnumerator() =>
        _array.IsDefault ? ImmutableArray<T>.Empty.GetEnumerator() : _array.GetEnumerator();

    IEnumerator<T> IEnumerable<T>.GetEnumerator()
    {
        if (_array.IsDefault)
            yield break;
        foreach (var item in _array)
            yield return item;
    }

    IEnumerator IEnumerable.GetEnumerator() => ((IEnumerable<T>)this).GetEnumerator();

    public static implicit operator EquatableArray<T>(ImmutableArray<T> array) => new(array);
}
