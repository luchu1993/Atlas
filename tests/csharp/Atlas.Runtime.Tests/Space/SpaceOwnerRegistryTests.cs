using System;
using Atlas.Entity;
using Atlas.Space;
using Xunit;

namespace Atlas.Runtime.Tests.SpaceTests;

public class SpaceOwnerRegistryTests : IDisposable
{
    public SpaceOwnerRegistryTests() => SpaceOwnerRegistry.ClearForTest();
    public void Dispose() => SpaceOwnerRegistry.ClearForTest();

    private sealed class FakeSpaceEntity : CellSpaceEntity
    {
        public override string TypeName => "FakeSpace";
        public override ushort TypeId => 999;
        public override void Serialize(ref Atlas.Serialization.SpanWriter w) { }
        public override void Deserialize(ref Atlas.Serialization.SpanReader r) { }
    }

    [Fact]
    public void Find_ReturnsNullWhenUnregistered()
    {
        Assert.Null(SpaceOwnerRegistry.Find(1));
    }

    [Fact]
    public void Register_StoresEntityForLater()
    {
        var e = new FakeSpaceEntity { EntityId = 100 };
        SpaceOwnerRegistry.Register(7, e);
        Assert.Same(e, SpaceOwnerRegistry.Find(7));
    }

    [Fact]
    public void RegisterSameEntityTwice_IsIdempotent()
    {
        var e = new FakeSpaceEntity { EntityId = 100 };
        SpaceOwnerRegistry.Register(7, e);
        SpaceOwnerRegistry.Register(7, e);  // no throw
        Assert.Same(e, SpaceOwnerRegistry.Find(7));
    }

    [Fact]
    public void RegisterDifferentEntityForSameSpace_Throws()
    {
        var a = new FakeSpaceEntity { EntityId = 100 };
        var b = new FakeSpaceEntity { EntityId = 200 };
        SpaceOwnerRegistry.Register(7, a);
        Assert.Throws<InvalidOperationException>(() => SpaceOwnerRegistry.Register(7, b));
    }

    [Fact]
    public void Unregister_RemovesEntity()
    {
        var e = new FakeSpaceEntity { EntityId = 100 };
        SpaceOwnerRegistry.Register(7, e);
        SpaceOwnerRegistry.Unregister(7, e);
        Assert.Null(SpaceOwnerRegistry.Find(7));
    }

    [Fact]
    public void Unregister_DifferentEntityIsNoop()
    {
        var a = new FakeSpaceEntity { EntityId = 100 };
        var b = new FakeSpaceEntity { EntityId = 200 };
        SpaceOwnerRegistry.Register(7, a);
        SpaceOwnerRegistry.Unregister(7, b);  // wrong entity — must keep `a`.
        Assert.Same(a, SpaceOwnerRegistry.Find(7));
    }
}
