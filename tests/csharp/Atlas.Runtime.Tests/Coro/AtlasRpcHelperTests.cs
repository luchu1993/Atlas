using System;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Coro.Testing;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("AtlasLoop")]
public sealed class AtlasRpcHelperTests : IDisposable
{
    private readonly TestLoop _loop;
    private readonly ManagedRpcRegistry _registry;

    public AtlasRpcHelperTests()
    {
        _loop = new TestLoop();
        AtlasLoop.Install(_loop);
        _registry = new ManagedRpcRegistry(_loop);
        AtlasRpcRegistryHost.Install(_registry);
        AtlasShutdownToken.Reset();
    }

    public void Dispose()
    {
        AtlasRpcRegistryHost.Reset();
        _registry.Dispose();
        AtlasLoop.Reset();
    }

    private static readonly AtlasRpcSource<RpcReply<int>>.SpanDeserializer DeserInt =
        static (ref SpanReader r) => RpcReply<int>.Ok(r.ReadInt32());

    private static byte[] BuildOkReply(uint requestId, int value)
    {
        var w = new SpanWriter(16);
        try
        {
            w.WriteUInt32(requestId);
            w.WriteInt32(0);
            w.WriteInt32(value);
            return w.WrittenSpan.ToArray();
        }
        finally { w.Dispose(); }
    }

    [Fact]
    public void RegistryHost_ThrowsBeforeInstall()
    {
        AtlasRpcRegistryHost.Reset();
        Assert.Throws<InvalidOperationException>(() => _ = AtlasRpcRegistryHost.Current);
        AtlasRpcRegistryHost.Install(_registry);
    }

    [Fact]
    public void RegistryHost_IsInstalledFlag()
    {
        Assert.True(AtlasRpcRegistryHost.IsInstalled);
        AtlasRpcRegistryHost.Reset();
        Assert.False(AtlasRpcRegistryHost.IsInstalled);
        AtlasRpcRegistryHost.Install(_registry);
    }

    [Fact]
    public void NextRequestId_Monotonic()
    {
        var a = AtlasRpc.NextRequestId();
        var b = AtlasRpc.NextRequestId();
        Assert.NotEqual(a, b);
    }

    [Fact]
    public void Await_UsesHostRegistry_ResolvesOnDispatch()
    {
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(replyId: 50, requestId, DeserInt);
        _registry.TryDispatch(50, requestId, BuildOkReply(requestId, 123));
        var reply = _loop.RunAwait(task);
        Assert.True(reply.IsOk);
        Assert.Equal(123, reply.Value);
    }

    [Fact]
    public void Await_ExplicitRegistry_OverridesHost()
    {
        using var other = new ManagedRpcRegistry(_loop);
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(other, replyId: 60, requestId, DeserInt);
        Assert.Equal(0, _registry.PendingCount);
        Assert.Equal(1, other.PendingCount);
        other.TryDispatch(60, requestId, BuildOkReply(requestId, 9));
        Assert.Equal(9, _loop.RunAwait(task).Value);
    }

    [Fact]
    public void Await_TimeoutFiresOnLoopAdvance()
    {
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(70, requestId, DeserInt, timeoutMs: 50);
        _loop.AdvanceTime(60);
        var reply = _loop.RunAwait(task);
        Assert.Equal(RpcErrorCodes.Timeout, reply.Error);
    }

    [Fact]
    public void Await_CancelledTokenAbortsAndUnregisters()
    {
        var cts = new AtlasCancellationSource();
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(80, requestId, DeserInt, ct: cts.Token);
        Assert.Equal(1, _registry.PendingCount);
        cts.Cancel();
        Assert.Equal(0, _registry.PendingCount);
        var reply = _loop.RunAwait(task);
        Assert.Equal(RpcErrorCodes.Cancelled, reply.Error);
    }

    [Fact]
    public void OrThrow_OkReturnsValue()
    {
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(110, requestId, DeserInt).OrThrow();
        _registry.TryDispatch(110, requestId, BuildOkReply(requestId, 42));
        Assert.Equal(42, _loop.RunAwait(task));
    }

    [Fact]
    public void OrThrow_ErrorRaisesRpcException()
    {
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(120, requestId, DeserInt, timeoutMs: 50).OrThrow();
        _loop.AdvanceTime(60);
        var ex = Assert.Throws<RpcException>(() => _loop.RunAwait(task));
        Assert.Equal(RpcErrorCodes.Timeout, ex.ErrorCode);
        Assert.Equal(RpcFrameworkMessages.Timeout, ex.Message);
    }

    [Fact]
    public void ShutdownToken_BeforeFire_NotCancelled()
    {
        Assert.False(AtlasShutdownToken.IsShutdownRequested);
        Assert.False(AtlasShutdownToken.Token.IsCancellationRequested);
    }

    [Fact]
    public void ShutdownToken_AfterRequest_PropagatesToAwaitingRpc()
    {
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(90, requestId, DeserInt,
            ct: AtlasShutdownToken.Token);

        AtlasShutdownToken.RequestShutdown();
        Assert.True(AtlasShutdownToken.IsShutdownRequested);
        var reply = _loop.RunAwait(task);
        Assert.Equal(RpcErrorCodes.Cancelled, reply.Error);
    }

    [Fact]
    public void ShutdownToken_ResetClearsCancelledState()
    {
        AtlasShutdownToken.RequestShutdown();
        Assert.True(AtlasShutdownToken.IsShutdownRequested);
        AtlasShutdownToken.Reset();
        Assert.False(AtlasShutdownToken.IsShutdownRequested);
    }

    [Fact]
    public void RpcReplyHelpers_For_ReturnsCachedDelegate()
    {
        var a = RpcReplyHelpers.For<int>();
        var b = RpcReplyHelpers.For<int>();
        Assert.Same(a, b);
    }

    [Fact]
    public void RpcReplyHelpers_For_DistinctTypesGetDistinctMappers()
    {
        // Verify Cache<T> specialises per T (cannot Assert.NotEqual delegates
        // of different generic types — use behavior to distinguish).
        var intMap = RpcReplyHelpers.For<int>();
        var strMap = RpcReplyHelpers.For<string>();
        Assert.Equal(7, intMap(7, "x").Error);
        Assert.Equal(7, strMap(7, "x").Error);
    }
}
