using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

[Entity("DirtyTestEntity")]
public partial class DirtyTestEntity : ServerEntity
{
    [Replicated] private int _score;
    [Replicated] private float _speed;
    [Replicated] private string _name = "";
}

public class DirtyTrackingTests
{
    [Fact]
    public void InitiallyClean()
    {
        var entity = new DirtyTestEntity();
        Assert.False(entity.IsDirty);
    }

    [Fact]
    public void SetProperty_MarksDirty()
    {
        var entity = new DirtyTestEntity();
        entity.Score = 42;
        Assert.True(entity.IsDirty);
    }

    [Fact]
    public void SetSameValue_StaysClean()
    {
        var entity = new DirtyTestEntity();
        entity.Score = 0;
        Assert.False(entity.IsDirty);
    }

    [Fact]
    public void ClearDirty_ResetsFlags()
    {
        var entity = new DirtyTestEntity();
        entity.Score = 10;
        entity.Speed = 5.5f;
        Assert.True(entity.IsDirty);
        entity.ClearDirty();
        Assert.False(entity.IsDirty);
    }

    [Fact]
    public void SerializeDelta_OnlyDirtyFields()
    {
        var entity = new DirtyTestEntity();
        entity.Score = 42;

        var writer = new SpanWriter(256);
        entity.SerializeReplicatedDelta(ref writer);

        var reader = new SpanReader(writer.WrittenSpan);
        var flags = reader.ReadByte();
        Assert.NotEqual((byte)0, flags);
        Assert.Equal(42, reader.ReadInt32());
        writer.Dispose();
    }

    [Fact]
    public void ApplyDelta_RestoresValues()
    {
        var source = new DirtyTestEntity();
        source.Score = 99;
        source.Name = "Test";

        var writer = new SpanWriter(256);
        source.SerializeReplicatedDelta(ref writer);

        var target = new DirtyTestEntity();
        var reader = new SpanReader(writer.WrittenSpan);
        target.ApplyReplicatedDelta(ref reader);

        Assert.Equal(99, target.Score);
        Assert.Equal("Test", target.Name);
        writer.Dispose();
    }
}
