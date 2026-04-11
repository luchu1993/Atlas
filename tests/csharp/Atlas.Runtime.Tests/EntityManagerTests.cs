using Atlas.Entity;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests;

/// <summary>
/// Serializes tests for the <see cref="EntityManager"/> singleton (shared process-wide state).
/// </summary>
[CollectionDefinition("EntityManager", DisableParallelization = true)]
public class EntityManagerCollection
{
}

[Collection("EntityManager")]
public class EntityManagerTests
{
    public EntityManagerTests()
    {
        EntityManager.Instance.Reset();
    }

    private class TestEntity : ServerEntity
    {
        public override string TypeName => "TestEntity";
        public int Health = 100;

        public override void Serialize(ref SpanWriter writer)
        {
            writer.WriteInt32(1); // version
            writer.WriteInt32(1); // fieldCount
            var body = new SpanWriter(64);
            try
            {
                body.WriteInt32(Health);
                writer.WriteInt32(body.Length);
                writer.WriteRawBytes(body.WrittenSpan);
            }
            finally { body.Dispose(); }
        }

        public override void Deserialize(ref SpanReader reader)
        {
            _ = reader.ReadInt32(); // version
            var fieldCount = reader.ReadInt32();
            var bodyLength = reader.ReadInt32();
            var bodyStart = reader.Position;
            if (fieldCount > 0) Health = reader.ReadInt32();
            var consumed = reader.Position - bodyStart;
            if (consumed < bodyLength) reader.Advance(bodyLength - consumed);
        }
    }

    [Fact]
    public void InitialCountIsZero()
    {
        var mgr = EntityManager.Instance;
        Assert.Equal(0, mgr.Count);
    }

    [Fact]
    public void CreateEntity_IncrementsCount()
    {
        var mgr = EntityManager.Instance;
        mgr.Create<TestEntity>();
        Assert.Equal(1, mgr.Count);
    }

    [Fact]
    public void Create_AssignsEntityId()
    {
        var mgr = EntityManager.Instance;
        var entity = mgr.Create<TestEntity>();
        Assert.NotEqual(0u, entity.EntityId);
    }

    [Fact]
    public void GetAllEntities_ReturnsRegistered()
    {
        var mgr = EntityManager.Instance;
        mgr.Create<TestEntity>();
        mgr.Create<TestEntity>();
        var all = mgr.GetAllEntities();
        Assert.Equal(2, all.Count);
    }

    [Fact]
    public void Clear_RemovesAll()
    {
        var mgr = EntityManager.Instance;
        mgr.Create<TestEntity>();
        mgr.Create<TestEntity>();
        mgr.Clear();
        Assert.Equal(0, mgr.Count);
    }

    [Fact]
    public void Reset_ClearsAll()
    {
        var mgr = EntityManager.Instance;
        mgr.Create<TestEntity>();
        mgr.Reset();
        Assert.Equal(0, mgr.Count);
    }

    [Fact]
    public void Destroy_RemovesEntity()
    {
        var mgr = EntityManager.Instance;
        var entity = mgr.Create<TestEntity>();
        mgr.Destroy(entity.EntityId);
        Assert.Equal(0, mgr.Count);
        Assert.True(entity.IsDestroyed);
    }

    [Fact]
    public void OnTickAll_CallsOnTick()
    {
        var mgr = EntityManager.Instance;
        var entity = mgr.Create<TrackingEntity>();
        mgr.OnTickAll(0.016f);
        Assert.Equal(1, entity.TickCount);
    }

    [Fact]
    public void OnInitAll_CallsOnInit()
    {
        var mgr = EntityManager.Instance;
        var entity = mgr.Create<TrackingEntity>();
        mgr.OnInitAll(false);
        Assert.True(entity.InitCalled);
    }

    [Fact]
    public void OnShutdownAll_CallsOnDestroy()
    {
        var mgr = EntityManager.Instance;
        var entity = mgr.Create<TrackingEntity>();
        mgr.OnShutdownAll();
        Assert.True(entity.DestroyCalled);
    }

    private class TrackingEntity : ServerEntity
    {
        public override string TypeName => "TrackingEntity";
        public int TickCount;
        public bool InitCalled;
        public bool DestroyCalled;

        public override void Serialize(ref SpanWriter writer)
        {
            writer.WriteInt32(1); // version
            writer.WriteInt32(0); // fieldCount
            writer.WriteInt32(0); // bodyLength
        }

        public override void Deserialize(ref SpanReader reader)
        {
            _ = reader.ReadInt32(); // version
            var fieldCount = reader.ReadInt32();
            var bodyLength = reader.ReadInt32();
            if (bodyLength > 0) reader.Advance(bodyLength);
        }

        protected internal override void OnTick(float dt) => TickCount++;
        protected internal override void OnInit(bool isReload) => InitCalled = true;
        protected internal override void OnDestroy() => DestroyCalled = true;
    }
}
