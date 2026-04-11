using System;

namespace Atlas.Runtime.Entity;

/// <summary>
/// Ring buffer of recent delta serialization snapshots for an entity.
/// Enables retransmission to clients that missed updates.
/// </summary>
public sealed class DeltaHistory
{
    private readonly DeltaEntry[] _entries;
    private uint _nextSeq = 1;
    private int _head;

    public DeltaHistory(int capacity = 32)
    {
        _entries = new DeltaEntry[capacity];
    }

    /// <summary>Record a new delta blob, returns its sequence number.</summary>
    public uint Record(ReadOnlySpan<byte> deltaBlob)
    {
        uint seq = _nextSeq++;
        ref var entry = ref _entries[_head];
        entry.Seq = seq;

        if (entry.Data == null || entry.Data.Length < deltaBlob.Length)
            entry.Data = new byte[deltaBlob.Length];

        deltaBlob.CopyTo(entry.Data);
        entry.Length = deltaBlob.Length;

        _head = (_head + 1) % _entries.Length;
        return seq;
    }

    /// <summary>
    /// Get all delta blobs since (exclusive) the given sequence number.
    /// Returns the concatenated delta data and the latest sequence number.
    /// </summary>
    public (byte[]? data, uint latestSeq) GetDeltasSince(uint sinceSeq)
    {
        if (_nextSeq <= 1) return (null, 0);

        uint latestSeq = _nextSeq - 1;
        if (sinceSeq >= latestSeq) return (null, latestSeq);

        int totalSize = 0;
        int count = 0;

        for (int i = 0; i < _entries.Length; i++)
        {
            ref var e = ref _entries[i];
            if (e.Seq > sinceSeq && e.Seq <= latestSeq && e.Data != null)
            {
                totalSize += e.Length;
                count++;
            }
        }

        if (count == 0) return (null, latestSeq);

        var result = new byte[totalSize];
        int offset = 0;

        for (uint s = sinceSeq + 1; s <= latestSeq; s++)
        {
            for (int i = 0; i < _entries.Length; i++)
            {
                ref var e = ref _entries[i];
                if (e.Seq == s && e.Data != null)
                {
                    Buffer.BlockCopy(e.Data, 0, result, offset, e.Length);
                    offset += e.Length;
                    break;
                }
            }
        }

        return (result, latestSeq);
    }

    public uint CurrentSeq => _nextSeq > 0 ? _nextSeq - 1 : 0;

    private struct DeltaEntry
    {
        public uint Seq;
        public byte[]? Data;
        public int Length;
    }
}
