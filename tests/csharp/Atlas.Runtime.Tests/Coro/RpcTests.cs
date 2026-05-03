using System;
using System.Collections.Generic;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Coro.Testing;
using Atlas.Serialization;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("AtlasLoop")]
public sealed class RpcTests : IDisposable
{
    private readonly TestLoop _loop;
    private readonly ManagedRpcRegistry _registry;

    public RpcTests()
    {
        _loop = new TestLoop();
        AtlasLoop.Install(_loop);
        _registry = new ManagedRpcRegistry(_loop);
    }

    public void Dispose()
    {
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

    private static byte[] BuildErrReply(uint requestId, int code, string msg)
    {
        var w = new SpanWriter(64);
        try
        {
            w.WriteUInt32(requestId);
            w.WriteInt32(code);
            w.WriteString(msg);
            return w.WrittenSpan.ToArray();
        }
        finally { w.Dispose(); }
    }

    // -------- AtlasRpcSource (success path) --------

    [Fact]
    public void RpcSource_Success_DeserializesAndCompletes()
    {
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, replyId: 100, requestId: 7,
            DeserInt, RpcReplyHelpers.For<int>(), timeoutMs: 5000);

        Assert.True(_registry.TryDispatch(100, 7, BuildOkReply(7, 42)));
        var reply = _loop.RunAwait(src.Task);
        Assert.True(reply.IsOk);
        Assert.Equal(42, reply.Value);
    }

    [Fact]
    public void RpcSource_BusinessError_PropagatesAsRpcReplyFail()
    {
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 8, DeserInt, RpcReplyHelpers.For<int>(), 5000);

        Assert.True(_registry.TryDispatch(100, 8, BuildErrReply(8, 42, "biz oops")));
        var reply = _loop.RunAwait(src.Task);
        Assert.True(reply.IsBusinessError);
        Assert.Equal(42, reply.Error);
        Assert.Equal("biz oops", reply.ErrorMessage);
    }

    [Fact]
    public void RpcSource_FrameworkError_FromReceiverGonePayload()
    {
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 9, DeserInt, RpcReplyHelpers.For<int>(), 5000);

        Assert.True(_registry.TryDispatch(100, 9,
            BuildErrReply(9, RpcErrorCodes.ReceiverGone, RpcFrameworkMessages.ReceiverGone)));
        var reply = _loop.RunAwait(src.Task);
        Assert.True(reply.IsFrameworkError);
        Assert.Equal(RpcErrorCodes.ReceiverGone, reply.Error);
    }

    // -------- AtlasRpcSource (framework error paths) --------

    [Fact]
    public void RpcSource_Timeout_RoutesThroughErrorMapper()
    {
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 10, DeserInt, RpcReplyHelpers.For<int>(), timeoutMs: 50);

        var task = src.Task;
        _loop.AdvanceTime(60);
        var reply = _loop.RunAwait(task);
        Assert.True(reply.IsFrameworkError);
        Assert.Equal(RpcErrorCodes.Timeout, reply.Error);
        Assert.Equal(RpcFrameworkMessages.Timeout, reply.ErrorMessage);
    }

    [Fact]
    public void RpcSource_PreCancelledToken_CompletesAsCancelledReply()
    {
        var cts = new AtlasCancellationSource();
        cts.Cancel();
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 11, DeserInt, RpcReplyHelpers.For<int>(), 5000, cts.Token);

        var reply = _loop.RunAwait(src.Task);
        Assert.True(reply.IsFrameworkError);
        Assert.Equal(RpcErrorCodes.Cancelled, reply.Error);
        Assert.Equal(0, _registry.PendingCount);
    }

    [Fact]
    public void RpcSource_CancelMidFlight_TearsDownPendingEntry()
    {
        var cts = new AtlasCancellationSource();
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 12, DeserInt, RpcReplyHelpers.For<int>(), 5000, cts.Token);

        Assert.Equal(1, _registry.PendingCount);
        cts.Cancel();
        Assert.Equal(0, _registry.PendingCount);
        var reply = _loop.RunAwait(src.Task);
        Assert.Equal(RpcErrorCodes.Cancelled, reply.Error);
    }

    [Fact]
    public void RpcSource_DeserializerThrows_MapsToPayloadMalformed()
    {
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 13,
            static (ref SpanReader r) => throw new FormatException("bad reply"),
            RpcReplyHelpers.For<int>(), 5000);

        Assert.True(_registry.TryDispatch(100, 13, BuildOkReply(13, 0)));
        var reply = _loop.RunAwait(src.Task);
        Assert.True(reply.IsFrameworkError);
        Assert.Equal(RpcErrorCodes.PayloadMalformed, reply.Error);
        Assert.Equal("bad reply", reply.ErrorMessage);
    }

    [Fact]
    public void RpcSource_ReceiverGone_RoutesThroughErrorMapper()
    {
        // Use a registry that lets the test trigger ReceiverGone directly so
        // the source's ErrorMapper is exercised without leaking a stale entry.
        var triggerReg = new TestTriggerRegistry();
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(triggerReg, 100, 15, DeserInt, RpcReplyHelpers.For<int>(), 5000);
        triggerReg.FireError(RpcCompletionStatus.ReceiverGone);

        var reply = _loop.RunAwait(src.Task);
        Assert.Equal(RpcErrorCodes.ReceiverGone, reply.Error);
        Assert.Equal(RpcFrameworkMessages.ReceiverGone, reply.ErrorMessage);
    }

    private sealed class TestTriggerRegistry : IAtlasRpcRegistry
    {
        private IAtlasRpcCallback? _cb;
        public long RegisterPending(int replyId, uint requestId, int timeoutMs, IAtlasRpcCallback cb)
        {
            _cb = cb;
            return 1;
        }
        public void CancelPending(long handle) { _cb = null; }
        public void FireError(RpcCompletionStatus status) { _cb?.OnError(status); _cb = null; }
    }

    [Fact]
    public void RpcSource_TruncatedHeader_MapsToPayloadMalformed()
    {
        var src = AtlasRpcSource<RpcReply<int>>.Rent();
        src.Start(_registry, 100, 14, DeserInt, RpcReplyHelpers.For<int>(), 5000);

        // Only 4 bytes (request_id) — error_code read will throw.
        var truncated = new byte[4];
        BitConverter.GetBytes((uint)14).CopyTo(truncated, 0);
        Assert.True(_registry.TryDispatch(100, 14, truncated));

        var reply = _loop.RunAwait(src.Task);
        Assert.True(reply.IsFrameworkError);
        Assert.Equal(RpcErrorCodes.PayloadMalformed, reply.Error);
    }

    // -------- ManagedRpcRegistry plumbing (unchanged) --------

    [Fact]
    public void Registry_DispatchMissingKey_ReturnsFalse()
    {
        Assert.False(_registry.TryDispatch(999, 999, ReadOnlySpan<byte>.Empty));
    }

    [Fact]
    public void Registry_CancelPending_FiresErrorCallback()
    {
        var captured = new List<RpcCompletionStatus>();
        var cb = new RecordingCallback(_ => { }, captured.Add);
        var h = _registry.RegisterPending(50, 1, timeoutMs: 5000, cb);
        _registry.CancelPending(h);
        Assert.Single(captured);
        Assert.Equal(RpcCompletionStatus.Cancelled, captured[0]);
    }

    [Fact]
    public void Registry_TimeoutFires_OnError()
    {
        var captured = new List<RpcCompletionStatus>();
        var cb = new RecordingCallback(_ => { }, captured.Add);
        _registry.RegisterPending(50, 1, timeoutMs: 100, cb);

        _loop.AdvanceTime(110);
        Assert.Single(captured);
        Assert.Equal(RpcCompletionStatus.Timeout, captured[0]);
        Assert.Equal(0, _registry.PendingCount);
    }

    [Fact]
    public void Registry_NoTimeoutWhenZero()
    {
        var captured = new List<RpcCompletionStatus>();
        var cb = new RecordingCallback(_ => { }, captured.Add);
        _registry.RegisterPending(50, 1, timeoutMs: 0, cb);

        _loop.AdvanceTime(60_000);
        Assert.Empty(captured);
        Assert.Equal(1, _registry.PendingCount);
    }

    // -------- End-to-end via async/await --------

    [Fact]
    public void EndToEnd_AwaitRpc_Succeeds()
    {
        async AtlasTask<int> AwaitReply(uint requestId)
        {
            var src = AtlasRpcSource<RpcReply<int>>.Rent();
            src.Start(_registry, 100, requestId, DeserInt, RpcReplyHelpers.For<int>(), 5000);
            var reply = await src.Task;
            return reply.Value;
        }

        var task = AwaitReply(123);
        Assert.Equal(1, _registry.PendingCount);
        _registry.TryDispatch(100, 123, BuildOkReply(123, 777));
        Assert.Equal(777, _loop.RunAwait(task));
    }

    [Fact]
    public void EndToEnd_AwaitRpc_Cancelled_ProducesCancelledReply()
    {
        var cts = new AtlasCancellationSource();
        async AtlasTask<RpcReply<int>> AwaitReply()
        {
            var src = AtlasRpcSource<RpcReply<int>>.Rent();
            src.Start(_registry, 100, 999, DeserInt, RpcReplyHelpers.For<int>(), 5000, cts.Token);
            return await src.Task;
        }

        var task = AwaitReply();
        Assert.Equal(1, _registry.PendingCount);
        cts.Cancel();
        Assert.Equal(0, _registry.PendingCount);
        var reply = _loop.RunAwait(task);
        Assert.Equal(RpcErrorCodes.Cancelled, reply.Error);
    }

    private delegate void OnReplyHandler(ReadOnlySpan<byte> payload);

    private sealed class RecordingCallback : IAtlasRpcCallback
    {
        private readonly OnReplyHandler _onReply;
        private readonly Action<RpcCompletionStatus> _onError;

        public RecordingCallback(OnReplyHandler onReply, Action<RpcCompletionStatus> onError)
        {
            _onReply = onReply;
            _onError = onError;
        }

        public void OnReply(ReadOnlySpan<byte> payload) => _onReply(payload);
        public void OnError(RpcCompletionStatus status) => _onError(status);
    }
}
