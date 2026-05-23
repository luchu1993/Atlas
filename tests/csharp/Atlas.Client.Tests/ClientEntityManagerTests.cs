using Xunit;

namespace Atlas.Client.Tests
{
    public class ClientEntityManagerTests
    {
        private sealed class TestEntity : ClientEntity
        {
            public override string TypeName => "Test";
            public int OnDestroyCalls;
            protected internal override void OnDestroy() => OnDestroyCalls++;
        }

        [Fact]
        public void ClearRemovesAllAndFiresOnDestroy()
        {
            var mgr = new ClientEntityManager();
            var a = new TestEntity { EntityId = 1 };
            var b = new TestEntity { EntityId = 2 };
            var removed = new System.Collections.Generic.List<uint>();
            mgr.EntityRemoved += entity => removed.Add(entity.EntityId);
            mgr.Register(a);
            mgr.Register(b);

            mgr.Clear();

            Assert.Equal(0, mgr.Count);
            Assert.Null(mgr.Get(1));
            Assert.Null(mgr.Get(2));
            Assert.True(a.IsDestroyed);
            Assert.True(b.IsDestroyed);
            Assert.Equal(1, a.OnDestroyCalls);
            Assert.Equal(1, b.OnDestroyCalls);
            Assert.Equal(new uint[] { 1, 2 }, removed);
        }

        [Fact]
        public void ClearOnEmptyManagerIsNoOp()
        {
            var mgr = new ClientEntityManager();
            mgr.Clear();
            Assert.Equal(0, mgr.Count);
        }

        [Fact]
        public void RegisterAndDestroyPublishEntityEvents()
        {
            var mgr = new ClientEntityManager();
            var added = new System.Collections.Generic.List<uint>();
            var removed = new System.Collections.Generic.List<uint>();
            mgr.EntityAdded += entity => added.Add(entity.EntityId);
            mgr.EntityRemoved += entity => removed.Add(entity.EntityId);

            mgr.Register(new TestEntity { EntityId = 7 });
            mgr.Destroy(7);

            Assert.Equal(new uint[] { 7 }, added);
            Assert.Equal(new uint[] { 7 }, removed);
        }
    }
}
