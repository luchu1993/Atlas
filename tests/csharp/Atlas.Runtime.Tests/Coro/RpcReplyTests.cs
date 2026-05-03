using System;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Coro.Testing;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("AtlasLoop")]
public sealed class RpcReplyTests : IDisposable
{
    private readonly TestLoop _loop;

    public RpcReplyTests()
    {
        _loop = new TestLoop();
        AtlasLoop.Install(_loop);
    }

    public void Dispose() => AtlasLoop.Reset();

    [Fact]
    public void Ok_HasZeroErrorAndExposesValue()
    {
        var r = RpcReply<int>.Ok(42);
        Assert.True(r.IsOk);
        Assert.False(r.IsBusinessError);
        Assert.False(r.IsFrameworkError);
        Assert.Equal(0, r.Error);
        Assert.Null(r.ErrorMessage);
        Assert.Equal(42, r.Value);
    }

    [Fact]
    public void Fail_BusinessCode_PositiveBranch()
    {
        var r = RpcReply<int>.Fail(7, "not found");
        Assert.False(r.IsOk);
        Assert.True(r.IsBusinessError);
        Assert.False(r.IsFrameworkError);
        Assert.Equal(7, r.Error);
        Assert.Equal("not found", r.ErrorMessage);
    }

    [Fact]
    public void Fail_FrameworkCode_NegativeBranch()
    {
        var r = RpcReply<int>.Fail(RpcErrorCodes.Timeout, RpcFrameworkMessages.Timeout);
        Assert.False(r.IsOk);
        Assert.False(r.IsBusinessError);
        Assert.True(r.IsFrameworkError);
        Assert.Equal(-1, r.Error);
        Assert.Same(RpcFrameworkMessages.Timeout, r.ErrorMessage);
    }

    [Fact]
    public void Value_OnError_Throws()
    {
        var r = RpcReply<string>.Fail(RpcErrorCodes.ReceiverGone, "gone");
        Assert.Throws<InvalidOperationException>(() => _ = r.Value);
    }

    [Fact]
    public void TryGetValue_OkReturnsTrue_FailReturnsFalse()
    {
        Assert.True(RpcReply<int>.Ok(5).TryGetValue(out var v));
        Assert.Equal(5, v);

        Assert.False(RpcReply<int>.Fail(2, "x").TryGetValue(out _));
    }

    [Fact]
    public void DefaultStruct_BehavesAsOkWithDefaultValue()
    {
        // default(RpcReply<int>) is technically Error == 0 with default value;
        // we don't promise this is meaningful but it must not throw on Value.
        RpcReply<int> r = default;
        Assert.True(r.IsOk);
        Assert.Equal(0, r.Value);
    }

    [Fact]
    public void ImplicitConversionToAtlasTask_Succeeds()
    {
        AtlasTask<RpcReply<int>> t = RpcReply<int>.Ok(99);
        var result = _loop.RunAwait(t);
        Assert.True(result.IsOk);
        Assert.Equal(99, result.Value);
    }

    [Fact]
    public void ImplicitConversionToAtlasTask_PropagatesError()
    {
        AtlasTask<RpcReply<string>> t = RpcReply<string>.Fail(13, "bad");
        var result = _loop.RunAwait(t);
        Assert.True(result.IsBusinessError);
        Assert.Equal(13, result.Error);
        Assert.Equal("bad", result.ErrorMessage);
    }

    [Fact]
    public void RpcErrorCodes_AllNegative()
    {
        Assert.True(RpcErrorCodes.Timeout < 0);
        Assert.True(RpcErrorCodes.Cancelled < 0);
        Assert.True(RpcErrorCodes.RemoteException < 0);
        Assert.True(RpcErrorCodes.ReceiverGone < 0);
        Assert.True(RpcErrorCodes.SendFailed < 0);
        Assert.True(RpcErrorCodes.MethodNotFound < 0);
        Assert.True(RpcErrorCodes.PayloadMalformed < 0);
    }
}
