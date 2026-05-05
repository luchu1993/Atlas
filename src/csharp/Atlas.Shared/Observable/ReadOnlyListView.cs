using System.Collections;
using System.Collections.Generic;

namespace Atlas.Observable;

// Read-only window over a List<T>. The struct GetEnumerator() keeps
// duck-typed foreach allocation-free; IReadOnlyList<T> still type-checks
// for LINQ / xUnit at the cost of one box per interface call.
public readonly struct ReadOnlyListView<T> : IReadOnlyList<T>
{
    private readonly List<T> _list;

    internal ReadOnlyListView(List<T> list) { _list = list; }

    public int Count => _list.Count;

    public T this[int index] => _list[index];

    public List<T>.Enumerator GetEnumerator() => _list.GetEnumerator();

    IEnumerator<T> IEnumerable<T>.GetEnumerator() => _list.GetEnumerator();

    IEnumerator IEnumerable.GetEnumerator() => _list.GetEnumerator();
}
