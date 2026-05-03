using System;
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Text;
using Atlas.DataTypes;

namespace Atlas.Serialization;

/// <summary>
/// Binary reader over a ReadOnlySpan&lt;byte&gt;. ref struct for zero GC pressure.
/// Wire format: little-endian, matching C++ BinaryReader byte-for-byte.
/// </summary>
public ref struct SpanReader
{
    private readonly ReadOnlySpan<byte> _data;
    private int _position;

    public SpanReader(ReadOnlySpan<byte> data)
    {
        _data = data;
        _position = 0;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public byte ReadByte()
    {
        CheckRemaining(1);
        return _data[_position++];
    }

    public bool ReadBool() => ReadByte() != 0;

    public sbyte ReadInt8() => (sbyte)ReadByte();

    public byte ReadUInt8() => ReadByte();

    public short ReadInt16()
    {
        CheckRemaining(2);
        var v = BinaryPrimitives.ReadInt16LittleEndian(_data.Slice(_position));
        _position += 2;
        return v;
    }

    public ushort ReadUInt16()
    {
        CheckRemaining(2);
        var v = BinaryPrimitives.ReadUInt16LittleEndian(_data.Slice(_position));
        _position += 2;
        return v;
    }

    public int ReadInt32()
    {
        CheckRemaining(4);
        var v = BinaryPrimitives.ReadInt32LittleEndian(_data.Slice(_position));
        _position += 4;
        return v;
    }

    public uint ReadUInt32()
    {
        CheckRemaining(4);
        var v = BinaryPrimitives.ReadUInt32LittleEndian(_data.Slice(_position));
        _position += 4;
        return v;
    }

    public long ReadInt64()
    {
        CheckRemaining(8);
        var v = BinaryPrimitives.ReadInt64LittleEndian(_data.Slice(_position));
        _position += 8;
        return v;
    }

    public ulong ReadUInt64()
    {
        CheckRemaining(8);
        var v = BinaryPrimitives.ReadUInt64LittleEndian(_data.Slice(_position));
        _position += 8;
        return v;
    }

    public float ReadFloat()
        => BitConverter.Int32BitsToSingle(ReadInt32());

    public double ReadDouble()
        => BitConverter.Int64BitsToDouble(ReadInt64());

    /// <summary>
    /// Three-tier packed_int matching C++ BinaryReader::read_packed_int:
    /// tag &lt; 0xFE → value = tag;
    /// tag == 0xFE → value = next uint16 LE;
    /// tag == 0xFF → value = next uint32 LE.
    /// </summary>
    public uint ReadPackedUInt32()
    {
        byte tag = ReadByte();
        if (tag < 0xFE) return tag;
        return tag == 0xFE ? ReadUInt16() : ReadUInt32();
    }

    /// <summary>
    /// String format matching C++ BinaryReader::read_string.
    /// </summary>
    public string ReadString()
    {
        int length = (int)ReadPackedUInt32();
        CheckRemaining(length);
        var str = Encoding.UTF8.GetString(_data.Slice(_position, length));
        _position += length;
        return str;
    }

    public Vector3 ReadVector3()
        => new(ReadFloat(), ReadFloat(), ReadFloat());

    public Quaternion ReadQuaternion()
        => new(ReadFloat(), ReadFloat(), ReadFloat(), ReadFloat());

    public ReadOnlySpan<byte> ReadBytes()
    {
        int length = (int)ReadPackedUInt32();
        CheckRemaining(length);
        var data = _data.Slice(_position, length);
        _position += length;
        return data;
    }

    /// <summary>Dequantize uint16 to yaw radian [0, 2π).</summary>
    public float ReadQuantizedYaw()
    {
        ushort quantized = ReadUInt16();
        const float TwoPi = MathF.PI * 2.0f;
        return quantized / 65536.0f * TwoPi;
    }

    /// <summary>Dequantize int8 to pitch radian [-π/2, π/2].</summary>
    public float ReadQuantizedPitch()
    {
        sbyte quantized = (sbyte)ReadByte();
        return quantized / 127.0f * (MathF.PI / 2);
    }

    public readonly int Remaining => _data.Length - _position;
    public readonly int Position => _position;

    /// <summary>Skip forward by <paramref name="count"/> bytes.</summary>
    public void Advance(int count)
    {
        CheckRemaining(count);
        _position += count;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void CheckRemaining(int count)
    {
        if (_position + count > _data.Length)
            throw new InvalidOperationException(
                $"SpanReader underflow: need {count} bytes at position {_position}, " +
                $"but only {_data.Length - _position} remaining");
    }
}
