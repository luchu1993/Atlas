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
        }

        [Fact]
        public void ClearOnEmptyManagerIsNoOp()
        {
            var mgr = new ClientEntityManager();
            mgr.Clear();
            Assert.Equal(0, mgr.Count);
        }
    }
}
