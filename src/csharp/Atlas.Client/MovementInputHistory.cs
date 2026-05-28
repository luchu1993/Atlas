using System;
using Atlas.Client.Native;

namespace Atlas.Client;

public sealed class MovementInputHistory
{
    readonly AtlasMovementInputFrame[] _frames;
    int _start;

    public MovementInputHistory(int capacity)
    {
        if (capacity <= 0) throw new ArgumentOutOfRangeException(nameof(capacity));
        _frames = new AtlasMovementInputFrame[capacity];
    }

    public int Count { get; private set; }
    public int Capacity => _frames.Length;

    public AtlasMovementInputFrame this[int index]
    {
        get
        {
            if ((uint)index >= (uint)Count) throw new ArgumentOutOfRangeException(nameof(index));
            return _frames[(_start + index) % _frames.Length];
        }
    }

    public void Clear()
    {
        _start = 0;
        Count = 0;
    }

    public void Push(AtlasMovementInputFrame input)
    {
        if (Count == _frames.Length)
        {
            _start = (_start + 1) % _frames.Length;
            --Count;
        }
        _frames[(_start + Count) % _frames.Length] = input;
        ++Count;
    }

    public void DropAcked(uint ackedSeq)
    {
        while (Count > 0 && !MovementSequence.IsNewer(this[0].Seq, ackedSeq))
        {
            _start = (_start + 1) % _frames.Length;
            --Count;
        }
    }

    public int CopyRecent(Span<AtlasMovementInputFrame> destination)
    {
        int count = Math.Min(destination.Length, Count);
        int first = Count - count;
        for (int i = 0; i < count; ++i)
            destination[i] = this[first + i];
        return count;
    }
}
