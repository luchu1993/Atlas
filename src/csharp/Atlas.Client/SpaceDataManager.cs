using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using System.Text;

namespace Atlas.Client;

// Per-client SpaceData mirror, fed by the kSpaceData* envelopes from
// the cellapp witness. Scripts subscribe via KeyChanged / KeyRemoved.
public sealed class SpaceDataManager
{
    private readonly Dictionary<(uint SpaceId, ushort KeyId), byte[]> _data = new();

    public event Action<uint, ushort, ReadOnlyMemory<byte>>? KeyChanged;
    public event Action<uint, ushort>? KeyRemoved;
    public event Action<uint>? Initialized;

    public bool TryGet(uint spaceId, ushort keyId, out ReadOnlyMemory<byte> value)
    {
        if (_data.TryGetValue((spaceId, keyId), out var raw))
        {
            value = raw;
            return true;
        }
        value = default;
        return false;
    }

    public IEnumerable<KeyValuePair<ushort, byte[]>> KeysOf(uint spaceId)
    {
        foreach (var kv in _data)
            if (kv.Key.SpaceId == spaceId) yield return new(kv.Key.KeyId, kv.Value);
    }

    // Typed readers — return default if missing or wire-truncated.
    public int GetInt32(uint spaceId, ushort keyId, int fallback = 0) =>
        TryGet(spaceId, keyId, out var v) && v.Length >= 4
            ? BinaryPrimitives.ReadInt32LittleEndian(v.Span) : fallback;

    public long GetInt64(uint spaceId, ushort keyId, long fallback = 0) =>
        TryGet(spaceId, keyId, out var v) && v.Length >= 8
            ? BinaryPrimitives.ReadInt64LittleEndian(v.Span) : fallback;

    public uint GetUInt32(uint spaceId, ushort keyId, uint fallback = 0) =>
        TryGet(spaceId, keyId, out var v) && v.Length >= 4
            ? BinaryPrimitives.ReadUInt32LittleEndian(v.Span) : fallback;

    public ulong GetUInt64(uint spaceId, ushort keyId, ulong fallback = 0) =>
        TryGet(spaceId, keyId, out var v) && v.Length >= 8
            ? BinaryPrimitives.ReadUInt64LittleEndian(v.Span) : fallback;

    public float GetFloat(uint spaceId, ushort keyId, float fallback = 0)
    {
        if (!TryGet(spaceId, keyId, out var v) || v.Length < 4) return fallback;
        var bits = BinaryPrimitives.ReadUInt32LittleEndian(v.Span);
        return BitConverter.Int32BitsToSingle(unchecked((int)bits));
    }

    public double GetDouble(uint spaceId, ushort keyId, double fallback = 0)
    {
        if (!TryGet(spaceId, keyId, out var v) || v.Length < 8) return fallback;
        var bits = BinaryPrimitives.ReadUInt64LittleEndian(v.Span);
        return BitConverter.Int64BitsToDouble(unchecked((long)bits));
    }

    public bool GetBool(uint spaceId, ushort keyId, bool fallback = false) =>
        TryGet(spaceId, keyId, out var v) && v.Length >= 1
            ? v.Span[0] != 0 : fallback;

    public string GetString(uint spaceId, ushort keyId, string fallback = "") =>
        TryGet(spaceId, keyId, out var v) ? Encoding.UTF8.GetString(v.Span) : fallback;

    internal void InitSpace(uint spaceId, List<(ushort KeyId, byte[] Value)> entries)
    {
        var stale = _data.Keys.Where(k => k.SpaceId == spaceId).ToList();
        foreach (var k in stale) _data.Remove(k);
        foreach (var e in entries) _data[(spaceId, e.KeyId)] = e.Value;
        Initialized?.Invoke(spaceId);
    }

    internal void SetKey(uint spaceId, ushort keyId, byte[] value)
    {
        _data[(spaceId, keyId)] = value;
        KeyChanged?.Invoke(spaceId, keyId, value);
    }

    internal void RemoveKey(uint spaceId, ushort keyId)
    {
        if (_data.Remove((spaceId, keyId))) KeyRemoved?.Invoke(spaceId, keyId);
    }

    // Test hook only — production never wipes mid-session.
    public void ClearForTest()
    {
        _data.Clear();
        KeyChanged = null;
        KeyRemoved = null;
        Initialized = null;
    }
}
