using System;
using System.Collections.Generic;

namespace Atlas.Observable;

// Keyed analog of ObservableList. Mutations record an op locally;
// nested Observable values bubble dirty state via MarkChildDirty so the
// recursive serializer can emit per-key inner streams without re-
// shipping covered keys.
public sealed class ObservableDict<TKey, TValue> : IObservableChild, System.Collections.IEnumerable
    where TKey : notnull
{
    public readonly struct DictOp
    {
        public readonly OpKind Kind;
        public readonly int KeyIndex;
        public readonly int ValueIndex;

        public DictOp(OpKind kind, int keyIndex, int valueIndex)
        {
            Kind = kind;
            KeyIndex = keyIndex;
            ValueIndex = valueIndex;
        }
    }

    private static readonly Action _NoOp = () => { };

    private readonly Dictionary<TKey, TValue> _items = new();
    private readonly List<DictOp> _ops = new();
    private readonly List<TKey> _opKeys = new();
    private readonly List<TValue> _opValues = new();

    // Lazy: dicts whose values aren't observable never call MarkChildDirty.
    private static readonly HashSet<TKey> _emptyChildDirty = new();
    private HashSet<TKey>? _childDirtyKeys;

    // Per-key markDirty closure cache. Keys typically have stable
    // lifetime within a dict, so caching by key keeps Attach allocations
    // to one-per-distinct-key over the dict's life. Pruned on Remove
    // so a key that comes-and-goes doesn't leak entries.
    private Dictionary<TKey, Action>? _keyMarkDirtyCache;

    private Action _markDirty;

    public ObservableDict() : this(null) { }
    public ObservableDict(Action? markDirty) { _markDirty = markDirty ?? _NoOp; }

    public int Count => _items.Count;

    public Dictionary<TKey, TValue>.Enumerator GetEnumerator() => _items.GetEnumerator();

    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() =>
        _items.GetEnumerator();

    public IReadOnlyDictionary<TKey, TValue> Items => _items;
    // ReadOnlyListView preserves the read-only API (no Add/Clear) while
    // exposing a struct GetEnumerator() so codegen's foreach over Ops
    // stays allocation-free.  Same rationale as ObservableList.Ops.
    public ReadOnlyListView<DictOp> Ops => new(_ops);
    public ReadOnlyListView<TKey> OpKeys => new(_opKeys);
    public ReadOnlyListView<TValue> OpValues => new(_opValues);

    // Concrete HashSet so the duck-typed foreach in generated code uses
    // the struct enumerator. Empty fallback shared and read-only.
    public HashSet<TKey> ChildDirtyKeys => _childDirtyKeys ?? _emptyChildDirty;

    public void ClearOpLog()
    {
        _ops.Clear();
        _opKeys.Clear();
        _opValues.Clear();
    }

    public void ClearChildDirty() => _childDirtyKeys?.Clear();

    void IObservableChild.__Rebind(Action markDirty) => _markDirty = markDirty ?? _NoOp;

    void IObservableChild.__ForceClear()
    {
        ClearOpLog();
        if (_childDirtyKeys == null || _childDirtyKeys.Count == 0) return;
        foreach (var key in _childDirtyKeys)
        {
            if (_items.TryGetValue(key, out var v) && v is IObservableChild ch) ch.__ForceClear();
        }
        _childDirtyKeys.Clear();
    }

    public void MarkChildDirty(TKey key)
    {
        (_childDirtyKeys ??= new HashSet<TKey>()).Add(key);
        _markDirty();
    }

    public TValue this[TKey key]
    {
        get => _items[key];
        set
        {
            if (_items.TryGetValue(key, out var existing) &&
                EqualityComparer<TValue>.Default.Equals(existing, value)) return;
            if (_items.TryGetValue(key, out var old)) DetachChild(old);
            _items[key] = value;
            AttachNewChild(key, value);
            var kIdx = _opKeys.Count;
            _opKeys.Add(key);
            var vIdx = _opValues.Count;
            _opValues.Add(value);
            _ops.Add(new DictOp(OpKind.DictSet, kIdx, vIdx));
            _markDirty();
        }
    }

    public void Add(TKey key, TValue value)
    {
        _items.Add(key, value);
        AttachNewChild(key, value);
        var kIdx = _opKeys.Count;
        _opKeys.Add(key);
        var vIdx = _opValues.Count;
        _opValues.Add(value);
        _ops.Add(new DictOp(OpKind.DictSet, kIdx, vIdx));
        _markDirty();
    }

    public bool Remove(TKey key)
    {
        if (!_items.TryGetValue(key, out var old)) return false;
        DetachChild(old);
        _items.Remove(key);
        _childDirtyKeys?.Remove(key);
        _keyMarkDirtyCache?.Remove(key);
        var kIdx = _opKeys.Count;
        _opKeys.Add(key);
        _ops.Add(new DictOp(OpKind.DictErase, kIdx, -1));
        _markDirty();
        return true;
    }

    public void Clear()
    {
        if (_items.Count == 0 && _ops.Count == 0) return;
        foreach (var kv in _items) DetachChild(kv.Value);
        _items.Clear();
        _ops.Clear();
        _opKeys.Clear();
        _opValues.Clear();
        _childDirtyKeys?.Clear();
        _keyMarkDirtyCache?.Clear();
        _ops.Add(new DictOp(OpKind.Clear, -1, -1));
        _markDirty();
    }

    public bool ContainsKey(TKey key) => _items.ContainsKey(key);
    public bool TryGetValue(TKey key, out TValue value) => _items.TryGetValue(key, out value!);

    // Pre-size the backing dictionary so apply-side reads of large
    // payloads avoid log(N) rehashes during AddWithoutDirty.
    public void EnsureCapacity(int capacity) => _items.EnsureCapacity(capacity);

    // ---- Apply path ----

    public void ClearWithoutDirty()
    {
        foreach (var kv in _items) DetachChild(kv.Value);
        _items.Clear();
    }

    public void AddWithoutDirty(TKey key, TValue value)
    {
        if (_items.TryGetValue(key, out var old)) DetachChild(old);
        _items[key] = value;
        AttachNewChild(key, value);
    }

    public void RemoveWithoutDirty(TKey key)
    {
        if (!_items.TryGetValue(key, out var old)) return;
        DetachChild(old);
        _items.Remove(key);
        _keyMarkDirtyCache?.Remove(key);
    }

    public void CompactOpLog() => CompactOpLog(keyWireBytes: 8, valueWireBytes: 4);

    // Byte-aware fallback. _opValues.Count is the count of DictSet ops
    // (only those push values); op kind + key payload is paid by every
    // op. Fallback writes a Clear plus one DictSet per remaining item.
    public void CompactOpLog(int keyWireBytes, int valueWireBytes)
    {
        if (_ops.Count >= 2) DedupPerKey();

        int oplogBytes = _ops.Count * (1 + keyWireBytes) + _opValues.Count * valueWireBytes;
        int fallbackBytes = 1 + _items.Count * (1 + keyWireBytes + valueWireBytes);
        if (oplogBytes > fallbackBytes) EmitIntegralRewriteAsOps();

        DedupChildDirtyAgainstOuterOps();
    }

    private void DedupChildDirtyAgainstOuterOps()
    {
        if (_childDirtyKeys == null || _childDirtyKeys.Count == 0) return;
        // Clear() pre-wiped _childDirtyKeys; whatever remains is from
        // post-Clear (or no Clear at all). Drain only keys covered by
        // Set/Erase ops — Clear doesn't cover any specific key.
        foreach (var op in _ops)
        {
            if (op.Kind == OpKind.DictSet || op.Kind == OpKind.DictErase)
                DrainCoveredKey(_opKeys[op.KeyIndex]);
        }
    }

    private void DrainCoveredKey(TKey key)
    {
        if (_childDirtyKeys == null || !_childDirtyKeys.Remove(key)) return;
        if (_items.TryGetValue(key, out var v) && v is IObservableChild ch) ch.__ForceClear();
    }

    private void DedupPerKey()
    {
        int startIdx = 0;
        if (_ops[0].Kind == OpKind.Clear) startIdx = 1;

        var lastIdx = new Dictionary<TKey, int>();
        for (int i = startIdx; i < _ops.Count; ++i)
            lastIdx[_opKeys[_ops[i].KeyIndex]] = i;
        if (lastIdx.Count == _ops.Count - startIdx) return;

        var newOps = new List<DictOp>();
        var newKeys = new List<TKey>();
        var newValues = new List<TValue>();
        if (startIdx == 1) newOps.Add(_ops[0]);

        for (int i = startIdx; i < _ops.Count; ++i)
        {
            var op = _ops[i];
            var key = _opKeys[op.KeyIndex];
            if (lastIdx[key] != i) continue;

            int newKeyIdx = newKeys.Count;
            newKeys.Add(key);
            int newValIdx = -1;
            if (op.Kind == OpKind.DictSet)
            {
                newValIdx = newValues.Count;
                newValues.Add(_opValues[op.ValueIndex]);
            }
            newOps.Add(new DictOp(op.Kind, newKeyIdx, newValIdx));
        }

        _ops.Clear(); _ops.AddRange(newOps);
        _opKeys.Clear(); _opKeys.AddRange(newKeys);
        _opValues.Clear(); _opValues.AddRange(newValues);
    }

    private void EmitIntegralRewriteAsOps()
    {
        _ops.Clear();
        _opKeys.Clear();
        _opValues.Clear();
        _ops.Add(new DictOp(OpKind.Clear, -1, -1));
        foreach (var kv in _items)
        {
            int kIdx = _opKeys.Count;
            _opKeys.Add(kv.Key);
            int vIdx = _opValues.Count;
            _opValues.Add(kv.Value);
            _ops.Add(new DictOp(OpKind.DictSet, kIdx, vIdx));
        }
    }

    // ---- Attach / detach helpers ----
    //
    // AttachNewChild drains stale ops from pre-adoption (collection-
    // initializer mutations etc.). Detach rebinds to no-op; orphans
    // continue to function but no longer fire upward.

    private void AttachNewChild(TKey key, TValue item)
    {
        if (item is IObservableChild child)
        {
            child.__ForceClear();
            child.__Rebind(GetCachedKeyMarkDirty(key));
        }
        else if (item is IRebindableFields fields)
        {
            fields.__RebindObservableFields(GetCachedKeyMarkDirty(key));
        }
    }

    private Action GetCachedKeyMarkDirty(TKey key)
    {
        _keyMarkDirtyCache ??= new Dictionary<TKey, Action>();
        if (_keyMarkDirtyCache.TryGetValue(key, out var existing)) return existing;
        var captured = key;
        Action created = () => MarkChildDirty(captured);
        _keyMarkDirtyCache[key] = created;
        return created;
    }

    private void DetachChild(TValue item)
    {
        if (item is IObservableChild child)
            child.__Rebind(_NoOp);
        else if (item is IRebindableFields fields)
            fields.__RebindObservableFields(_NoOp);
    }
}
