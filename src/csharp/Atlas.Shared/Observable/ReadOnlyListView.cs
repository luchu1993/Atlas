using System.Collections;
using System.Collections.Generic;

namespace Atlas.Observable;

/// <summary>
/// Zero-allocation read-only view over a <see cref="List{T}"/>.  Pairs the
/// read-only API (Count + indexer) with a struct <c>GetEnumerator()</c>
/// so duck-typed <c>foreach</c> stays on the stack — neither the
/// IEnumerable.GetEnumerator boxing the bare interface incurs, nor the
/// Add/Clear surface a bare List<T> would expose.
/// </summary>
/// <remarks>
/// Used by <see cref="ObservableList{T}.Ops"/> / OpValues and
/// <see cref="ObservableDict{TKey, TValue}.Ops"/> / OpKeys / OpValues
/// to expose the internal mutation logs to the delta-sync codegen
/// without leaking mutating methods.  The constructor is internal so
/// the wrappers can only be minted from inside Atlas.Shared.
///
/// <see cref="IReadOnlyList{T}"/> is also implemented so call sites that
/// need an interface — LINQ helpers, xUnit's <c>Assert.All</c>, etc. —
/// still type-check.  Those paths box (the runtime cost of going
/// through an interface on a struct), but the codegen hot path stays
/// allocation-free because the C# compiler prefers the instance
/// <c>GetEnumerator()</c> overload during foreach pattern resolution.
/// </remarks>
public readonly struct ReadOnlyListView<T> : IReadOnlyList<T>
{
    private readonly List<T> _list;

    internal ReadOnlyListView(List<T> list) { _list = list; }

    public int Count => _list.Count;

    public T this[int index] => _list[index];

    /// <summary>
    /// Returns the underlying <see cref="List{T}.Enumerator"/> directly so
    /// <c>foreach</c> resolves to the struct enumerator and stays
    /// allocation-free.  Routing through <see cref="IEnumerable{T}"/>
    /// would box this struct once per call.
    /// </summary>
    public List<T>.Enumerator GetEnumerator() => _list.GetEnumerator();

    IEnumerator<T> IEnumerable<T>.GetEnumerator() => _list.GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => _list.GetEnumerator();
}
