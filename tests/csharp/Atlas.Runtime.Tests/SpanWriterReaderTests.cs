using System;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

public class SpanWriterReaderTests
{
    [Fact]
    public void WriteReadByte()
    {
        var writer = new SpanWriter(64);
        writer.WriteByte(0xAB);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(0xAB, reader.ReadByte());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadBool()
    {
        var writer = new SpanWriter(64);
        writer.WriteBool(true);
        writer.WriteBool(false);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.True(reader.ReadBool());
        Assert.False(reader.ReadBool());
        writer.Dispose();
    }

    [Theory]
    [InlineData((sbyte)0)]
    [InlineData((sbyte)127)]
    [InlineData((sbyte)-128)]
    [InlineData((sbyte)-1)]
    public void WriteReadInt8_RoundTrip(sbyte value)
    {
        var writer = new SpanWriter(64);
        writer.WriteInt8(value);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(value, reader.ReadInt8());
        writer.Dispose();
    }

    [Theory]
    [InlineData((byte)0)]
    [InlineData((byte)128)]
    [InlineData((byte)255)]
    public void WriteReadUInt8_RoundTrip(byte value)
    {
        var writer = new SpanWriter(64);
        writer.WriteUInt8(value);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(value, reader.ReadUInt8());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadInt32()
    {
        var writer = new SpanWriter(64);
        writer.WriteInt32(int.MinValue);
        writer.WriteInt32(0);
        writer.WriteInt32(int.MaxValue);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(int.MinValue, reader.ReadInt32());
        Assert.Equal(0, reader.ReadInt32());
        Assert.Equal(int.MaxValue, reader.ReadInt32());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadUInt32()
    {
        var writer = new SpanWriter(64);
        writer.WriteUInt32(0u);
        writer.WriteUInt32(uint.MaxValue);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(0u, reader.ReadUInt32());
        Assert.Equal(uint.MaxValue, reader.ReadUInt32());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadInt64()
    {
        var writer = new SpanWriter(64);
        writer.WriteInt64(long.MinValue);
        writer.WriteInt64(long.MaxValue);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(long.MinValue, reader.ReadInt64());
        Assert.Equal(long.MaxValue, reader.ReadInt64());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadFloat()
    {
        var writer = new SpanWriter(64);
        writer.WriteFloat(3.14f);
        writer.WriteFloat(float.NaN);
        writer.WriteFloat(float.PositiveInfinity);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(3.14f, reader.ReadFloat());
        Assert.True(float.IsNaN(reader.ReadFloat()));
        Assert.True(float.IsPositiveInfinity(reader.ReadFloat()));
        writer.Dispose();
    }

    [Fact]
    public void WriteReadDouble()
    {
        var writer = new SpanWriter(64);
        writer.WriteDouble(2.71828);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(2.71828, reader.ReadDouble());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadString_Empty()
    {
        var writer = new SpanWriter(64);
        writer.WriteString("");
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal("", reader.ReadString());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadString_Ascii()
    {
        var writer = new SpanWriter(64);
        writer.WriteString("Hello Atlas!");
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal("Hello Atlas!", reader.ReadString());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadString_Unicode()
    {
        var writer = new SpanWriter(256);
        writer.WriteString("中文测试🎮");
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal("中文测试🎮", reader.ReadString());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadPackedUInt32_Small()
    {
        var writer = new SpanWriter(64);
        writer.WritePackedUInt32(42);
        Assert.Equal(1, writer.Length);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(42u, reader.ReadPackedUInt32());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadPackedUInt32_Large()
    {
        var writer = new SpanWriter(64);
        writer.WritePackedUInt32(100000);
        Assert.Equal(5, writer.Length);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(100000u, reader.ReadPackedUInt32());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadBytes()
    {
        var writer = new SpanWriter(256);
        byte[] data = { 0x01, 0x02, 0x03, 0xFF };
        writer.WriteBytes(data);
        var reader = new SpanReader(writer.WrittenSpan);
        var result = reader.ReadBytes();
        Assert.Equal(data.Length, result.Length);
        for (int i = 0; i < data.Length; i++)
            Assert.Equal(data[i], result[i]);
        writer.Dispose();
    }

    [Fact]
    public void WriteReadRawBytes()
    {
        var writer = new SpanWriter(64);
        byte[] raw = { 0xDE, 0xAD, 0xBE, 0xEF };
        writer.WriteRawBytes(raw);
        Assert.Equal(4, writer.Length);
        var reader = new SpanReader(writer.WrittenSpan);
        Assert.Equal(0xDE, reader.ReadByte());
        Assert.Equal(0xAD, reader.ReadByte());
        Assert.Equal(0xBE, reader.ReadByte());
        Assert.Equal(0xEF, reader.ReadByte());
        writer.Dispose();
    }

    [Fact]
    public void ReaderAdvance()
    {
        var writer = new SpanWriter(64);
        writer.WriteInt32(1);
        writer.WriteInt32(2);
        writer.WriteInt32(3);
        var reader = new SpanReader(writer.WrittenSpan);
        reader.Advance(4);
        Assert.Equal(2, reader.ReadInt32());
        writer.Dispose();
    }

    [Fact]
    public void ReaderUnderflow_Throws()
    {
        var arr = new byte[] { 0x01 };
        Assert.Throws<InvalidOperationException>(() =>
        {
            var reader = new SpanReader(arr);
            reader.ReadInt32();
        });
    }

    [Fact]
    public void WriterGrowsAutomatically()
    {
        var writer = new SpanWriter(4);
        for (int i = 0; i < 100; i++)
            writer.WriteInt32(i);
        Assert.Equal(400, writer.Length);
        var reader = new SpanReader(writer.WrittenSpan);
        for (int i = 0; i < 100; i++)
            Assert.Equal(i, reader.ReadInt32());
        writer.Dispose();
    }

    [Fact]
    public void WriteReadVector3()
    {
        var writer = new SpanWriter(64);
        var v = new Atlas.DataTypes.Vector3(1.0f, 2.0f, 3.0f);
        writer.WriteVector3(v);
        var reader = new SpanReader(writer.WrittenSpan);
        var result = reader.ReadVector3();
        Assert.Equal(1.0f, result.X);
        Assert.Equal(2.0f, result.Y);
        Assert.Equal(3.0f, result.Z);
        writer.Dispose();
    }

    [Fact]
    public void WriteReadQuaternion()
    {
        var writer = new SpanWriter(64);
        var q = new Atlas.DataTypes.Quaternion(0.1f, 0.2f, 0.3f, 0.4f);
        writer.WriteQuaternion(q);
        var reader = new SpanReader(writer.WrittenSpan);
        var result = reader.ReadQuaternion();
        Assert.Equal(0.1f, result.X);
        Assert.Equal(0.2f, result.Y);
        Assert.Equal(0.3f, result.Z);
        Assert.Equal(0.4f, result.W);
        writer.Dispose();
    }
}
