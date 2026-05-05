using System;
using System.Collections.Generic;

namespace Atlas.Observable;

// Container-level dirty tracking with pass-through for nested observable
// children — see docs/property_sync/container_property_sync_design.md.
public sealed class ObservableList<T> : IObservableChild, System.Collections.IEnumerable
{
    public readonly struct ListOp
    {
        public readonly OpKind Kind;
        public readonly int Start;
        public readonly int End;
        public readonly int ValueOffset;
        public readonly int ValueCount;

        public ListOp(OpKind kind, int start, int end, int valueOffset, int valueCount)
        {
            Kind = kind;
            Start = start;
            End = end;
            ValueOffset = valueOffset;
            ValueCount = valueCount;
        }
    }

    private static readonly Action _NoOp = () => { };

    private readonly List<T> _items = new();
    private readonly List<ListOp> _ops = new();
    private readonly List<T> _opValues = new();

    // Lazy: scalar/struct lists never fire MarkChildDirty so the HashSet
    // stays null and skips ~100B fixed cost per instance.
    private static readonly HashSet<int> _emptyChildDirty = new();
    private HashSet<int>? _childDirtySlots;

    private List<Action>? _slotMarkDirtyCache;

    private Action _markDirty;

    public ObservableList() : this(null) { }
    public ObservableList(Action? markDirty) { _markDirty = markDirty ?? _NoOp; }

    public int Count => _items.Count;

    // Concrete return types so duck-typed foreach in codegen stays
    // allocation-free instead of boxing through IEnumerable<T>.
    public List<T>.Enumerator GetEnumerator() => _items.GetEnumerator();

    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() =>
        _items.GetEnumerator();

    public IReadOnlyList<T> Items => _items;
    public ReadOnlyListView<ListOp> Ops => new(_ops);
    public ReadOnlyListView<T> OpValues => new(_opValues);

    public HashSet<int> ChildDirtySlots => _childDirtySlots ?? _emptyChildDirty;

    public void ClearOpLog()
    {
        _ops.Clear();
        _opValues.Clear();
    }

    public void ClearChildDirty() => _childDirtySlots?.Clear();

    void IObservableChild.__Rebind(Action markDirty) => _markDirty = markDirty ?? _NoOp;

    void IObservableChild.__ForceClear()
    {
        ClearOpLog();
        if (_childDirtySlots == null || _childDirtySlots.Count == 0) return;
        foreach (var slot in _childDirtySlots)
        {
            if (slot < 0 || slot >= _items.Count) continue;
            if (_items[slot] is IObservableChild ch) ch.__ForceClear();
        }
        _childDirtySlots.Clear();
    }

    public void MarkChildDirty(int slotIdx)
    {
        (_childDirtySlots ??= new HashSet<int>()).Add(slotIdx);
        _markDirty();
    }

    public T this[int index]
    {
        get => _items[index];
        set
        {
            if (EqualityComparer<T>.Default.Equals(_items[index], value)) return;
            DetachChild(_items[index]);
            _items[index] = value;
            AttachNewChild(index, value);
            var offset = _opValues.Count;
            _opValues.Add(value);
            _ops.Add(new ListOp(OpKind.Set, index, index + 1, offset, 1));
            _markDirty();
        }
    }

    public void Add(T item)
    {
        var start = _items.Count;
        _items.Add(item);
        AttachNewChild(start, item);
        var offset = _opValues.Count;
        _opValues.Add(item);
        _ops.Add(new ListOp(OpKind.ListSplice, start, start, offset, 1));
        _markDirty();
    }

    public bool Remove(T item)
    {
        int idx = _items.IndexOf(item);
        if (idx < 0) return false;
        RemoveAt(idx);
        return true;
    }

    public void RemoveAt(int index)
    {
        DetachChild(_items[index]);
        _items.RemoveAt(index);
        ShiftChildDirtyForRemove(index);
        _ops.Add(new ListOp(OpKind.ListSplice, index, index + 1, 0, 0));
        // Surviving children shifted down — their captured slot indices are stale.
        RebindShiftedSlots(index);
        _markDirty();
    }

    public void Clear()
    {
        if (_items.Count == 0 && _ops.Count == 0) return;
        foreach (var item in _items) DetachChild(item);
        _items.Clear();
        _ops.Clear();
        _opValues.Clear();
        _childDirtySlots?.Clear();
        _ops.Add(new ListOp(OpKind.Clear, 0, 0, 0, 0));
        _markDirty();
    }

    public void Insert(int index, T item)
    {
        _items.Insert(index, item);
        ShiftChildDirtyForInsert(index);
        // Rebind shifted children BEFORE attaching the new item at `index`.
        RebindShiftedSlots(index + 1);
        AttachNewChild(index, item);
        var offset = _opValues.Count;
        _opValues.Add(item);
        _ops.Add(new ListOp(OpKind.ListSplice, index, index, offset, 1));
        _markDirty();
    }

    public bool Contains(T item) => _items.Contains(item);
    public int IndexOf(T item) => _items.IndexOf(item);

    // Snapshots the whole struct into _opValues so later structural
    // mutations can't drift the slot before serialize reaches this op.
    public void RecordStructFieldSet(int slot, byte fieldIdx)
    {
        var offset = _opValues.Count;
        _opValues.Add(_items[slot]);
        _ops.Add(new ListOp(OpKind.StructFieldSet, slot, fieldIdx, offset, 1));
        _markDirty();
    }

    public void ClearWithoutDirty()
    {
        foreach (var item in _items) DetachChild(item);
        _items.Clear();
    }

    public void AddWithoutDirty(T item)
    {
        _items.Add(item);
        AttachNewChild(_items.Count - 1, item);
    }

    public void SetWithoutDirty(int index, T value)
    {
        DetachChild(_items[index]);
        _items[index] = value;
        AttachNewChild(index, value);
    }

    public void InsertWithoutDirty(int index, T item)
    {
        _items.Insert(index, item);
        RebindShiftedSlots(index + 1);
        AttachNewChild(index, item);
    }

    public void RemoveRangeWithoutDirty(int start, int end)
    {
        if (end <= start) return;
        for (int i = start; i < end; ++i) DetachChild(_items[i]);
        _items.RemoveRange(start, end - start);
        RebindShiftedSlots(start);
    }

    public void EnsureCapacity(int capacity)
    {
        if (_items.Capacity < capacity) _items.Capacity = capacity;
    }

    public void CompactOpLog() => CompactOpLog(elementWireBytes: 4);

    // elementWireBytes lets the byte-aware fallback compare op-log size
    // vs integral-rewrite size correctly across narrow and wide elements.
    public void CompactOpLog(int elementWireBytes)
    {
        if (_ops.Count >= 2) MergeAdjacentAppends();

        int oplogBytes = _ops.Count * 7 + _opValues.Count * elementWireBytes;
        int fallbackBytes = 8 + _items.Count * elementWireBytes;
        if (oplogBytes > fallbackBytes) EmitIntegralRewriteAsOps();

        DedupChildDirtyAgainstOuterOps();
    }

    private void DedupChildDirtyAgainstOuterOps()
    {
        if (_childDirtySlots == null || _childDirtySlots.Count == 0) return;
        // Clear() already wiped _childDirtySlots; remaining entries are
        // post-Clear, so drain whichever slots the outer ops cover.
        foreach (var op in _ops)
        {
            if (op.Kind == OpKind.Set)
            {
                DrainCoveredSlot(op.Start);
            }
            else if (op.Kind == OpKind.ListSplice)
            {
                for (int s = op.Start; s < op.Start + op.ValueCount; ++s)
                    DrainCoveredSlot(s);
            }
        }
    }

    private void DrainCoveredSlot(int slot)
    {
        if (_childDirtySlots == null || !_childDirtySlots.Remove(slot)) return;
        if (slot < 0 || slot >= _items.Count) return;
        if (_items[slot] is IObservableChild ch) ch.__ForceClear();
    }

    private void MergeAdjacentAppends()
    {
        int w = 0;
        for (int r = 0; r < _ops.Count; ++r)
        {
            var cur = _ops[r];
            if (w > 0)
            {
                var prev = _ops[w - 1];
                bool bothPureInsertSplice =
                    prev.Kind == OpKind.ListSplice && cur.Kind == OpKind.ListSplice
                    && prev.Start == prev.End
                    && cur.Start == cur.End;
                bool indicesContiguous =
                    cur.Start == prev.Start + prev.ValueCount;
                bool valuesContiguous =
                    cur.ValueOffset == prev.ValueOffset + prev.ValueCount;
                if (bothPureInsertSplice && indicesContiguous && valuesContiguous)
                {
                    _ops[w - 1] = new ListOp(OpKind.ListSplice, prev.Start, prev.End,
                                             prev.ValueOffset,
                                             prev.ValueCount + cur.ValueCount);
                    continue;
                }
            }
            _ops[w++] = cur;
        }
        if (w < _ops.Count) _ops.RemoveRange(w, _ops.Count - w);
    }

    private void EmitIntegralRewriteAsOps()
    {
        _ops.Clear();
        _opValues.Clear();
        _ops.Add(new ListOp(OpKind.Clear, 0, 0, 0, 0));
        if (_items.Count == 0) return;
        var offset = _opValues.Count;
        _opValues.AddRange(_items);
        _ops.Add(new ListOp(OpKind.ListSplice, 0, 0, offset, _items.Count));
    }

    // AttachNewChild drains pre-adoption ops; RebindChildSlot must NOT —
    // a same-ref-shifted-slot still has legitimate ops queued under its
    // old slot that need to ship under the new one.
    private void AttachNewChild(int slotIdx, T item)
    {
        if (item is IObservableChild child)
        {
            child.__ForceClear();
            child.__Rebind(GetCachedSlotMarkDirty(slotIdx));
        }
        else if (item is IRebindableFields fields)
        {
            fields.__RebindObservableFields(GetCachedSlotMarkDirty(slotIdx));
        }
    }

    private void RebindChildSlot(int slotIdx, T item)
    {
        if (item is IObservableChild child)
            child.__Rebind(GetCachedSlotMarkDirty(slotIdx));
        else if (item is IRebindableFields fields)
            fields.__RebindObservableFields(GetCachedSlotMarkDirty(slotIdx));
    }

    // Per-slot closure cache: `() => MarkChildDirty(s)` allocates once
    // per slot ever used, not per mutation.
    private Action GetCachedSlotMarkDirty(int slot)
    {
        _slotMarkDirtyCache ??= new List<Action>();
        while (_slotMarkDirtyCache.Count <= slot)
            _slotMarkDirtyCache.Add(null!);
        var existing = _slotMarkDirtyCache[slot];
        if (existing != null) return existing;
        var captured = slot;
        Action created = () => MarkChildDirty(captured);
        _slotMarkDirtyCache[slot] = created;
        return created;
    }

    private void DetachChild(T item)
    {
        if (item is IObservableChild child)
            child.__Rebind(_NoOp);
        else if (item is IRebindableFields fields)
            fields.__RebindObservableFields(_NoOp);
    }

    private void RebindShiftedSlots(int startIdx)
    {
        for (int i = startIdx; i < _items.Count; ++i)
            RebindChildSlot(i, _items[i]);
    }

    private void ShiftChildDirtyForRemove(int removedIdx)
    {
        if (_childDirtySlots == null || _childDirtySlots.Count == 0) return;
        _childDirtySlots.Remove(removedIdx);
        ShiftChildDirty(threshold: removedIdx, delta: -1);
    }

    private void ShiftChildDirtyForInsert(int insertedIdx)
    {
        if (_childDirtySlots == null || _childDirtySlots.Count == 0) return;
        ShiftChildDirty(threshold: insertedIdx, delta: +1, includeThreshold: true);
    }

    private void ShiftChildDirty(int threshold, int delta, bool includeThreshold = false)
    {
        // Two-phase to avoid mutating the set during enumeration.
        var set = _childDirtySlots!;
        List<int>? toShift = null;
        foreach (var s in set)
        {
            bool match = includeThreshold ? s >= threshold : s > threshold;
            if (match) (toShift ??= new()).Add(s);
        }
        if (toShift == null) return;
        foreach (var s in toShift) set.Remove(s);
        foreach (var s in toShift) set.Add(s + delta);
    }
}
