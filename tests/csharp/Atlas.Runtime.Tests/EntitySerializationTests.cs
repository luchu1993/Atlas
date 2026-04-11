using Atlas.Serialization;
using Atlas.Entity;
using Xunit;

namespace Atlas.Tests;

public class EntitySerializationTests
{
    private class SimpleEntity : ServerEntity
    {
        public override string TypeName => "SimpleEntity";
        public int IntField = 0;
        public float FloatField = 0f;
        public string StringField = "";

        public override void Serialize(ref SpanWriter writer)
        {
            writer.WriteByte(1); // version
            writer.WriteUInt16(3); // fieldCount
            var body = new SpanWriter(128);
            try
            {
                body.WriteInt32(IntField);
                body.WriteFloat(FloatField);
                body.WriteString(StringField);
                writer.WriteUInt16((ushort)body.Length); // bodyLength
                writer.WriteRawBytes(body.WrittenSpan);
            }
            finally { body.Dispose(); }
        }

        public override void Deserialize(ref SpanReader reader)
        {
            _ = reader.ReadByte(); // version
            var fieldCount = reader.ReadUInt16();
            var bodyLength = reader.ReadUInt16();
            var bodyStart = reader.Position;
            if (fieldCount > 0) IntField = reader.ReadInt32();
            if (fieldCount > 1) FloatField = reader.ReadFloat();
            if (fieldCount > 2) StringField = reader.ReadString();
            var consumed = reader.Position - bodyStart;
            if (consumed < bodyLength) reader.Advance(bodyLength - consumed);
        }
    }

    [Fact]
    public void SerializeDeserialize_RoundTrip()
    {
        var entity = new SimpleEntity
        {
            IntField = 42,
            FloatField = 3.14f,
            StringField = "Hello"
        };

        var writer = new SpanWriter(256);
        entity.Serialize(ref writer);

        var restored = new SimpleEntity();
        var reader = new SpanReader(writer.WrittenSpan);
        restored.Deserialize(ref reader);

        Assert.Equal(42, restored.IntField);
        Assert.Equal(3.14f, restored.FloatField);
        Assert.Equal("Hello", restored.StringField);
        writer.Dispose();
    }

    [Fact]
    public void SerializeDeserialize_EmptyString()
    {
        var entity = new SimpleEntity { StringField = "" };
        var writer = new SpanWriter(256);
        entity.Serialize(ref writer);

        var restored = new SimpleEntity();
        var reader = new SpanReader(writer.WrittenSpan);
        restored.Deserialize(ref reader);

        Assert.Equal("", restored.StringField);
        writer.Dispose();
    }

    [Fact]
    public void SerializeDeserialize_UnicodeString()
    {
        var entity = new SimpleEntity { StringField = "你好世界🌍" };
        var writer = new SpanWriter(256);
        entity.Serialize(ref writer);

        var restored = new SimpleEntity();
        var reader = new SpanReader(writer.WrittenSpan);
        restored.Deserialize(ref reader);

        Assert.Equal("你好世界🌍", restored.StringField);
        writer.Dispose();
    }

    [Fact]
    public void SerializeDeserialize_DefaultValues()
    {
        var entity = new SimpleEntity();
        var writer = new SpanWriter(256);
        entity.Serialize(ref writer);

        var restored = new SimpleEntity { IntField = 99, FloatField = 99f, StringField = "old" };
        var reader = new SpanReader(writer.WrittenSpan);
        restored.Deserialize(ref reader);

        Assert.Equal(0, restored.IntField);
        Assert.Equal(0f, restored.FloatField);
        Assert.Equal("", restored.StringField);
        writer.Dispose();
    }

    [Fact]
    public void SerializeDeserialize_NegativeValues()
    {
        var entity = new SimpleEntity
        {
            IntField = int.MinValue,
            FloatField = float.NegativeInfinity
        };
        var writer = new SpanWriter(256);
        entity.Serialize(ref writer);

        var restored = new SimpleEntity();
        var reader = new SpanReader(writer.WrittenSpan);
        restored.Deserialize(ref reader);

        Assert.Equal(int.MinValue, restored.IntField);
        Assert.True(float.IsNegativeInfinity(restored.FloatField));
        writer.Dispose();
    }

    [Fact]
    public void TypeName_IsCorrect()
    {
        var entity = new SimpleEntity();
        Assert.Equal("SimpleEntity", entity.TypeName);
    }

    /// <summary>
    /// Simulates deserializing data that was serialized with fewer fields (old version).
    /// The entity should use defaults for missing fields.
    /// </summary>
    [Fact]
    public void Deserialize_FewerFields_UsesDefaults()
    {
        // Manually write a snapshot with only 1 field (IntField)
        var writer = new SpanWriter(256);
        writer.WriteByte(1); // version
        writer.WriteUInt16(1); // fieldCount = 1 (only IntField)
        var body = new SpanWriter(64);
        try
        {
            body.WriteInt32(777);
            writer.WriteUInt16((ushort)body.Length);
            writer.WriteRawBytes(body.WrittenSpan);
        }
        finally { body.Dispose(); }

        var restored = new SimpleEntity { IntField = 0, FloatField = 99f, StringField = "stale" };
        var reader = new SpanReader(writer.WrittenSpan);
        restored.Deserialize(ref reader);

        Assert.Equal(777, restored.IntField);
        Assert.Equal(99f, restored.FloatField); // unchanged — fieldCount < 2
        Assert.Equal("stale", restored.StringField); // unchanged — fieldCount < 3
        writer.Dispose();
    }

    /// <summary>
    /// Simulates deserializing data that was serialized with more fields (newer version).
    /// The extra trailing bytes should be safely skipped.
    /// </summary>
    [Fact]
    public void Deserialize_MoreFields_SkipsExtraBytes()
    {
        // Write a snapshot with 5 fields (3 known + 2 extra)
        var writer = new SpanWriter(256);
        writer.WriteByte(2); // version = 2 (future)
        writer.WriteUInt16(5); // fieldCount = 5
        var body = new SpanWriter(128);
        try
        {
            body.WriteInt32(42);
            body.WriteFloat(1.5f);
            body.WriteString("ok");
            body.WriteInt64(9999); // unknown field 1
            body.WriteBool(true);  // unknown field 2
            writer.WriteUInt16((ushort)body.Length);
            writer.WriteRawBytes(body.WrittenSpan);
        }
        finally { body.Dispose(); }

        var restored = new SimpleEntity();
        var reader = new SpanReader(writer.WrittenSpan);
        restored.Deserialize(ref reader);

        Assert.Equal(42, restored.IntField);
        Assert.Equal(1.5f, restored.FloatField);
        Assert.Equal("ok", restored.StringField);
        Assert.Equal(0, reader.Remaining); // all bytes consumed
        writer.Dispose();
    }
}
