using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Text;
using Atlas.DataTypes;

namespace Atlas.Serialization;

// Wire format matches C++ BinaryWriter byte-for-byte (little-endian).
public ref struct SpanWriter
{
    private byte[] _buffer;
    private int _position;

    // Explicit parameterless ctor so `new SpanWriter()` rents a live buffer
    // instead of zero-initing _buffer to null and NREing on first write.
    public SpanWriter() : this(256) {}

    public SpanWriter(int initialCapacity)
    {
        _buffer = ArrayPool<byte>.Shared.Rent(initialCapacity);
        _position = 0;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void WriteByte(byte value)
    {
        EnsureCapacity(1);
        _buffer[_position++] = value;
    }

    public void WriteBool(bool value) => WriteByte(value ? (byte)1 : (byte)0);

    public void WriteInt8(sbyte value) => WriteByte((byte)value);

    public void WriteUInt8(byte value) => WriteByte(value);

    public void WriteInt16(short value)
    {
        EnsureCapacity(2);
        BinaryPrimitives.WriteInt16LittleEndian(_buffer.AsSpan(_position), value);
        _position += 2;
    }

    public void WriteUInt16(ushort value)
    {
        EnsureCapacity(2);
        BinaryPrimitives.WriteUInt16LittleEndian(_buffer.AsSpan(_position), value);
        _position += 2;
    }

    public void WriteInt32(int value)
    {
        EnsureCapacity(4);
        BinaryPrimitives.WriteInt32LittleEndian(_buffer.AsSpan(_position), value);
        _position += 4;
    }

    public void WriteUInt32(uint value)
    {
        EnsureCapacity(4);
        BinaryPrimitives.WriteUInt32LittleEndian(_buffer.AsSpan(_position), value);
        _position += 4;
    }

    public void WriteInt64(long value)
    {
        EnsureCapacity(8);
        BinaryPrimitives.WriteInt64LittleEndian(_buffer.AsSpan(_position), value);
        _position += 8;
    }

    public void WriteUInt64(ulong value)
    {
        EnsureCapacity(8);
        BinaryPrimitives.WriteUInt64LittleEndian(_buffer.AsSpan(_position), value);
        _position += 8;
    }

    public void WriteFloat(float value)
        => WriteInt32(BitConverter.SingleToInt32Bits(value));

    public void WriteDouble(double value)
        => WriteInt64(BitConverter.DoubleToInt64Bits(value));

    // Three-tier packed_int: < 0xFE → 1 byte; ≤ 0xFFFF → 0xFE + u16; else 0xFF + u32.
    public void WritePackedUInt32(uint value)
    {
        if (value < 0xFE)
        {
            WriteByte((byte)value);
        }
        else if (value <= 0xFFFF)
        {
            WriteByte(0xFE);
            WriteUInt16((ushort)value);
        }
        else
        {
            WriteByte(0xFF);
            WriteUInt32(value);
        }
    }

    // packed_int(utf8_byte_length) + UTF-8 bytes.
    public void WriteString(string value)
    {
        if (value == null) value = "";
        int byteCount = Encoding.UTF8.GetByteCount(value);
        WritePackedUInt32((uint)byteCount);
        EnsureCapacity(byteCount);
        Encoding.UTF8.GetBytes(value.AsSpan(), _buffer.AsSpan(_position));
        _position += byteCount;
    }

    public void WriteVector3(Vector3 value)
    {
        WriteFloat(value.X);
        WriteFloat(value.Y);
        WriteFloat(value.Z);
    }

    public void WriteQuaternion(Quaternion value)
    {
        WriteFloat(value.X);
        WriteFloat(value.Y);
        WriteFloat(value.Z);
        WriteFloat(value.W);
    }

    public void WriteBytes(ReadOnlySpan<byte> data)
    {
        WritePackedUInt32((uint)data.Length);
        EnsureCapacity(data.Length);
        data.CopyTo(_buffer.AsSpan(_position));
        _position += data.Length;
    }

    // Quantize radian [0, 2π) → uint16.
    public void WriteQuantizedYaw(float radians)
    {
        const float TwoPi = MathF.PI * 2.0f;
        float normalized = ((radians % TwoPi) + TwoPi) % TwoPi;
        ushort quantized = (ushort)(normalized / TwoPi * 65536.0f);
        WriteUInt16(quantized);
    }

    // Quantize radian [-π/2, π/2] → int8.
    public void WriteQuantizedPitch(float radians)
    {
        float clamped = Math.Clamp(radians, -MathF.PI / 2, MathF.PI / 2);
        sbyte quantized = (sbyte)(clamped / (MathF.PI / 2) * 127.0f);
        WriteByte((byte)quantized);
    }

    // Write raw bytes without a length prefix.
    public void WriteRawBytes(ReadOnlySpan<byte> data)
    {
        EnsureCapacity(data.Length);
        data.CopyTo(_buffer.AsSpan(_position));
        _position += data.Length;
    }

    public readonly ReadOnlySpan<byte> WrittenSpan => _buffer.AsSpan(0, _position);
    public readonly int Length => _position;
    public readonly int Position => _position;

    // Rewinds position to 0; keeps the rented buffer so per-entity
    // serialise loops avoid ArrayPool.Rent traffic on the hot path.
    public void Reset() => _position = 0;

    public void Dispose()
    {
        if (_buffer != null)
        {
            ArrayPool<byte>.Shared.Return(_buffer);
            _buffer = null!;
        }
    }

    private void EnsureCapacity(int additional)
    {
        int required = _position + additional;
        if (required <= _buffer.Length) return;

        int newSize = Math.Max(_buffer.Length * 2, required);
        var newBuffer = ArrayPool<byte>.Shared.Rent(newSize);
        _buffer.AsSpan(0, _position).CopyTo(newBuffer);
        ArrayPool<byte>.Shared.Return(_buffer);
        _buffer = newBuffer;
    }
}
