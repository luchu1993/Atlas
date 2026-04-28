using System;
using System.Collections.Generic;

namespace Atlas.Observable;

// Growable list with dirty tracking at the container level AND pass-
// through to nested observable children. Mutations record an op locally;
// nested ObservableList / ObservableDict children bubble their dirty
// state via MarkChildDirty so the recursive serializer can emit a
// targeted op into the inner stream instead of re-shipping the whole
// slot.
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

    // Slots whose inner containers have pending ops. Lazy: lists with
    // scalar / struct elements never call MarkChildDirty (only nested
    // observable children fire it), so the HashSet stays null and we
    // skip ~100B of fixed cost per such instance. Code paths that read
    // _childDirtySlots fall through cleanly when it's null.
    private static readonly HashSet<int> _emptyChildDirty = new();
    private HashSet<int>? _childDirtySlots;

    // Per-slot markDirty closure cache. Each closure is allocated once
    // per slot ever used and reused on every Attach hitting that slot —
    // so a steady-state Add / RemoveAt cycle only pays the closure cost
    // the first time a fresh slot index appears.
    private List<Action>? _slotMarkDirtyCache;

    private Action _markDirty;

    public ObservableList() : this(null) { }
    public ObservableList(Action? markDirty) { _markDirty = markDirty ?? _NoOp; }

    public int Count => _items.Count;

    // Concrete return type so the compiler picks duck-typed foreach over
    // the IEnumerable interface — keeps the hot path zero-alloc.
    public List<T>.Enumerator GetEnumerator() => _items.GetEnumerator();

    // Non-generic IEnumerable is required by C# collection-initializer
    // syntax. foreach prefers the struct-returning overload above.
    System.Collections.IEnumerator System.Collections.IEnumerable.GetEnumerator() =>
        _items.GetEnumerator();

    public IReadOnlyList<T> Items => _items;
    // ReadOnlyListView wraps the underlying List<T> so:
    //   • foreach (var op in container.Ops)  uses the struct enumerator
    //     (no IEnumerable<T>.GetEnumerator boxing — historical hot
    //     allocation source flagged by dotnet-trace gc-verbose).
    //   • container.Ops.Count / container.Ops[i] stay constant-time.
    //   • There is no Add / Clear surface, so external readers cannot
    //     corrupt the delta log even though the wrapper has access to
    //     the live backing list.
    public ReadOnlyListView<ListOp> Ops => new(_ops);
    public ReadOnlyListView<T> OpValues => new(_opValues);

    // Concrete HashSet return so the duck-typed foreach in generated
    // code uses the struct enumerator (zero-alloc) instead of boxing
    // through IEnumerable<int>. Empty fallback is shared and never
    // mutated — never expose it for Add/Remove externally.
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
        // Parent's outer op already ships our integral state. Drain so
        // next tick doesn't re-emit folded-in mutations as new deltas.
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
        // Surviving children at index..count-1 shifted down — their
        // markDirty closures captured stale slot indices.
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
        // Existing items at index+1..count-1 shifted up — rebind their
        // captured slot indices BEFORE the new item attaches at `index`.
        RebindShiftedSlots(index + 1);
        AttachNewChild(index, item);
        var offset = _opValues.Count;
        _opValues.Add(item);
        _ops.Add(new ListOp(OpKind.ListSplice, index, index, offset, 1));
        _markDirty();
    }

    public bool Contains(T item) => _items.Contains(item);
    public int IndexOf(T item) => _items.IndexOf(item);

    // Records that one field of the struct at `slot` changed. The op
    // snapshots the whole struct into _opValues so subsequent
    // structural mutations (RemoveAt etc.) can't drift the slot index
    // before serialize reaches this op — the writer reads from
    // OpValues directly, not from _items.
    //
    // Called by the generator-emitted ItemAt accessor for list-of-
    // Field-mode-struct properties.
    public void RecordStructFieldSet(int slot, byte fieldIdx)
    {
        var offset = _opValues.Count;
        _opValues.Add(_items[slot]);
        _ops.Add(new ListOp(OpKind.StructFieldSet, slot, fieldIdx, offset, 1));
        _markDirty();
    }

    // ---- Apply path -----------------------------------------------------
    //
    // Client-side apply routes here. Bind / unbind still runs so that
    // future client-local mutations (prediction, test fixtures) report
    // correctly; WithoutDirty variants skip the op log + markDirty since
    // server is authoritative.

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

    // ---- Compaction (called by generator before serialize) --------------

    public void CompactOpLog() => CompactOpLog(elementWireBytes: 4);

    // Byte-aware fallback. Generator passes the element's estimated
    // wire width so we can compare op-log bytes to the integral rewrite
    // bytes properly. Without this hint, wide-element lists fallback
    // too eagerly and narrow-element lists fallback too late.
    //
    // op-log byte estimate = ops × (kind + Splice-header worst case 6)
    //                       + opValues × elementBytes
    // fallback estimate    = Clear (1) + Splice header (7)
    //                       + items × elementBytes
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
        // Clear() already nuked _childDirtySlots when it ran; any markers
        // here are post-Clear (legitimate fresh child mutations). Walk
        // the ops and drain whichever slots are covered by the outer
        // payload — that's Set's exact slot or Splice's inserted range.
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
            // Clear: no per-op action — see comment above.
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

    // ---- Attach / detach helpers ----------------------------------------
    //
    // AttachNewChild = adoption (new ref entering this list). Drain stale
    // ops from before adoption so collection-initializer mutations don't
    // re-apply on the receiver.
    //
    // RebindChildSlot = same ref staying, just at a new index. NEVER
    // drain — the child may hold legitimate ops registered against its
    // old slot that still need to ship under the new slot.

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

    // Lazy per-slot closure cache. The capture `() => MarkChildDirty(s)`
    // is the cheapest way to get a typed callback into the child without
    // changing IObservableChild's signature; caching by slot keeps that
    // alloc one-time-per-slot instead of per-mutation.
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

    // ---- Slot-shift bookkeeping for ChildDirtySlots ---------------------
    //
    // In-place rewriting via two cursors avoids the snapshot allocation
    // the previous List<int> clone-and-rebuild incurred per structural
    // change. Walks the bucket once and overwrites entries in-place;
    // HashSet<int>'s iterator tolerates Add/Remove only via this
    // pull-then-push ordering.

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
        // Find slots needing shift, then apply. Two-phase to avoid
        // mutating the set during enumeration.
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
