using System;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Coro.Testing;
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

    private static int DeserializeInt(ReadOnlySpan<byte> payload) =>
        BitConverter.ToInt32(payload.Slice(4, 4));

    private static byte[] BuildIntReply(uint requestId, int value)
    {
        var buf = new byte[8];
        BitConverter.GetBytes(requestId).CopyTo(buf.AsSpan(0, 4));
        BitConverter.GetBytes(value).CopyTo(buf.AsSpan(4, 4));
        return buf;
    }

    [Fact]
    public void RegistryHost_ThrowsBeforeInstall()
    {
        AtlasRpcRegistryHost.Reset();
        Assert.Throws<InvalidOperationException>(() => _ = AtlasRpcRegistryHost.Current);
        AtlasRpcRegistryHost.Install(_registry);   // restore for Dispose
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
        var task = AtlasRpc.Await<int>(replyId: 50, requestId, DeserializeInt);
        _registry.TryDispatch(50, requestId, BuildIntReply(requestId, 123));
        Assert.Equal(123, _loop.RunAwait(task));
    }

    [Fact]
    public void Await_ExplicitRegistry_OverridesHost()
    {
        using var other = new ManagedRpcRegistry(_loop);
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(other, replyId: 60, requestId, DeserializeInt);
        // Dispatch through the explicit registry, not the host one.
        Assert.Equal(0, _registry.PendingCount);
        Assert.Equal(1, other.PendingCount);
        other.TryDispatch(60, requestId, BuildIntReply(requestId, 9));
        Assert.Equal(9, _loop.RunAwait(task));
    }

    [Fact]
    public void Await_TimeoutFiresOnLoopAdvance()
    {
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(70, requestId, DeserializeInt, timeoutMs: 50);
        _loop.AdvanceTime(60);
        Assert.Throws<TimeoutException>(() => _loop.RunAwait(task));
    }

    [Fact]
    public void Await_CancelledTokenAbortsAndUnregisters()
    {
        var cts = new AtlasCancellationSource();
        var requestId = AtlasRpc.NextRequestId();
        var task = AtlasRpc.Await<int>(80, requestId, DeserializeInt, ct: cts.Token);
        Assert.Equal(1, _registry.PendingCount);
        cts.Cancel();
        Assert.Equal(0, _registry.PendingCount);
        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(task));
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
        var task = AtlasRpc.Await<int>(90, requestId, DeserializeInt,
            ct: AtlasShutdownToken.Token);

        AtlasShutdownToken.RequestShutdown();
        Assert.True(AtlasShutdownToken.IsShutdownRequested);
        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(task));
    }

    [Fact]
    public void ShutdownToken_ResetClearsCancelledState()
    {
        AtlasShutdownToken.RequestShutdown();
        Assert.True(AtlasShutdownToken.IsShutdownRequested);
        AtlasShutdownToken.Reset();
        Assert.False(AtlasShutdownToken.IsShutdownRequested);
    }
}
