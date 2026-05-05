using System;
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Text;
using Atlas.DataTypes;

namespace Atlas.Serialization;

// Wire format matches C++ BinaryReader byte-for-byte (little-endian).
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

    // Three-tier packed_int: tag < 0xFE → tag itself; 0xFE → next u16; 0xFF → next u32.
    public uint ReadPackedUInt32()
    {
        byte tag = ReadByte();
        if (tag < 0xFE) return tag;
        return tag == 0xFE ? ReadUInt16() : ReadUInt32();
    }

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

    // Dequantize uint16 → yaw radian [0, 2π).
    public float ReadQuantizedYaw()
    {
        ushort quantized = ReadUInt16();
        const float TwoPi = MathF.PI * 2.0f;
        return quantized / 65536.0f * TwoPi;
    }

    // Dequantize int8 → pitch radian [-π/2, π/2].
    public float ReadQuantizedPitch()
    {
        sbyte quantized = (sbyte)ReadByte();
        return quantized / 127.0f * (MathF.PI / 2);
    }

    public readonly int Remaining => _data.Length - _position;
    public readonly int Position => _position;

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
