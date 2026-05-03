using System;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Coro.Rpc;
using Atlas.Entity;
using Atlas.Hosting;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("EntityManager")]
public sealed class HotReloadDrainTests : IDisposable
{
    private readonly ManagedAtlasLoop _loop;
    private readonly ManagedRpcRegistry _registry;

    public HotReloadDrainTests()
    {
        EntityManager.Instance.Reset();
        _loop = new ManagedAtlasLoop();
        AtlasLoop.Install(_loop);
        _registry = new ManagedRpcRegistry(_loop);
    }

    public void Dispose()
    {
        _registry.Dispose();
        AtlasLoop.Reset();
        _loop.Dispose();
        EntityManager.Instance.Reset();
    }

    private sealed class Dummy : ServerEntity
    {
        public override string TypeName => "Dummy";
        public override void Serialize(ref SpanWriter w) { }
        public override void Deserialize(ref SpanReader r) { }
    }

    private static readonly AtlasRpcSource<RpcReply<int>>.SpanDeserializer Deser =
        static (ref SpanReader r) => RpcReply<int>.Ok(r.ReadInt32());

    [Fact]
    public void CancelLifecyclesAndDrain_CancelsInFlightAndRunsContinuations()
    {
        var e = EntityManager.Instance.Create<Dummy>();
        int observed = -1;
        async AtlasTask Await()
        {
            var src = AtlasRpcSource<RpcReply<int>>.Rent();
            src.Start(_registry, replyId: 100, requestId: 1,
                Deser, RpcReplyHelpers.For<int>(),
                timeoutMs: 5000, ct: e.LifecycleCancellation);
            var reply = await src.Task;
            observed = reply.Error;
        }
        _ = Await();

        Assert.Equal(1, _registry.PendingCount);
        HotReloadManager.CancelLifecyclesAndDrain(EntityManager.Instance.GetAllEntities());

        Assert.Equal(0, _registry.PendingCount);
        Assert.Equal(RpcErrorCodes.Cancelled, observed);
    }

    [Fact]
    public void CancelLifecyclesAndDrain_NoEntities_NoOp()
    {
        HotReloadManager.CancelLifecyclesAndDrain(EntityManager.Instance.GetAllEntities());
    }

    [Fact]
    public void CancelLifecyclesAndDrain_RpcWithoutLifecycleToken_LeavesPendingForTimeout()
    {
        var e = EntityManager.Instance.Create<Dummy>();
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 2, Deser, RpcReplyHelpers.For<int>(),
            timeoutMs: 5000);   // no LifecycleCancellation

        Assert.Equal(1, _registry.PendingCount);
        HotReloadManager.CancelLifecyclesAndDrain(EntityManager.Instance.GetAllEntities());
        // Drain cannot cancel an RPC the user did not link to the entity —
        // the registry will clear it via Dispose at the test's teardown.
        Assert.Equal(1, _registry.PendingCount);
    }
}
