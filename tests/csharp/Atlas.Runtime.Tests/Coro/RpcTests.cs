using System;
using System.Collections.Generic;
using System.Text;
using Atlas.Coro;
using Atlas.Coro.Rpc;
using Atlas.Coro.Testing;
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

    private static int DeserializeInt(ReadOnlySpan<byte> payload)
    {
        if (payload.Length < 8) throw new ArgumentException("payload too short");
        // First 4 bytes are request_id (we ignore here), next 4 are int value.
        return BitConverter.ToInt32(payload.Slice(4, 4));
    }

    private static byte[] BuildIntReply(uint requestId, int value)
    {
        var buf = new byte[8];
        BitConverter.GetBytes(requestId).CopyTo(buf.AsSpan(0, 4));
        BitConverter.GetBytes(value).CopyTo(buf.AsSpan(4, 4));
        return buf;
    }

    // -------- AtlasRpcSource --------

    [Fact]
    public void RpcSource_Success_DeserializesAndCompletes()
    {
        var src = AtlasRpcSource<int>.Rent();
        src.Start(_registry, replyId: 100, requestId: 7, DeserializeInt, timeoutMs: 5000);

        Assert.True(_registry.TryDispatch(100, 7, BuildIntReply(7, 42)));
        Assert.Equal(42, _loop.RunAwait(src.Task));
    }

    [Fact]
    public void RpcSource_Timeout_ThrowsTimeoutException()
    {
        var src = AtlasRpcSource<int>.Rent();
        src.Start(_registry, replyId: 100, requestId: 8, DeserializeInt, timeoutMs: 50);

        var task = src.Task;
        _loop.AdvanceTime(60);                     // fires the timer
        _loop.RunNextFrame();                      // not strictly needed (timer fires inline)
        Assert.Throws<TimeoutException>(() => _loop.RunAwait(task));
    }

    [Fact]
    public void RpcSource_PreCancelledToken_CompletesCancelledImmediately()
    {
        var cts = new AtlasCancellationSource();
        cts.Cancel();
        var src = AtlasRpcSource<int>.Rent();
        src.Start(_registry, replyId: 100, requestId: 9, DeserializeInt, timeoutMs: 5000, cts.Token);

        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(src.Task));
        Assert.Equal(0, _registry.PendingCount);
    }

    [Fact]
    public void RpcSource_CancelMidFlight_TearsDownPendingEntry()
    {
        var cts = new AtlasCancellationSource();
        var src = AtlasRpcSource<int>.Rent();
        src.Start(_registry, replyId: 100, requestId: 10, DeserializeInt, timeoutMs: 5000, cts.Token);

        Assert.Equal(1, _registry.PendingCount);
        cts.Cancel();
        Assert.Equal(0, _registry.PendingCount);
        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(src.Task));
    }

    [Fact]
    public void RpcSource_DeserializerThrows_PropagatesException()
    {
        var src = AtlasRpcSource<int>.Rent();
        src.Start(_registry, replyId: 100, requestId: 11,
            static _ => throw new FormatException("bad reply"), timeoutMs: 5000);

        Assert.True(_registry.TryDispatch(100, 11, BuildIntReply(11, 0)));
        Assert.Throws<FormatException>(() => _loop.RunAwait(src.Task));
    }

    // -------- ManagedRpcRegistry --------

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
    public void Registry_DuplicateKey_OldEntryCancelled()
    {
        var firstStatus = new List<RpcCompletionStatus>();
        var first = new RecordingCallback(_ => { }, firstStatus.Add);
        _registry.RegisterPending(50, 1, timeoutMs: 5000, first);

        var secondStatus = new List<RpcCompletionStatus>();
        var second = new RecordingCallback(_ => { }, secondStatus.Add);
        _registry.RegisterPending(50, 1, timeoutMs: 5000, second);

        Assert.Single(firstStatus);
        Assert.Equal(RpcCompletionStatus.Cancelled, firstStatus[0]);
        Assert.Empty(secondStatus);
        Assert.Equal(1, _registry.PendingCount);
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

    [Fact]
    public void Registry_Dispose_DrainsAllAsCancelled()
    {
        var cap1 = new List<RpcCompletionStatus>();
        var cap2 = new List<RpcCompletionStatus>();
        _registry.RegisterPending(50, 1, 5000, new RecordingCallback(_ => { }, cap1.Add));
        _registry.RegisterPending(50, 2, 5000, new RecordingCallback(_ => { }, cap2.Add));

        _registry.Dispose();
        Assert.Equal(RpcCompletionStatus.Cancelled, Assert.Single(cap1));
        Assert.Equal(RpcCompletionStatus.Cancelled, Assert.Single(cap2));
    }

    [Fact]
    public void Registry_TryDispatch_PassesPayloadAndUnregisters()
    {
        ReadOnlyMemory<byte>? captured = null;
        var cb = new RecordingCallback(payload =>
        {
            captured = payload.ToArray();
        }, _ => { });

        _registry.RegisterPending(50, 1, 5000, cb);
        var payload = Encoding.UTF8.GetBytes("hello");
        Assert.True(_registry.TryDispatch(50, 1, payload));
        Assert.NotNull(captured);
        Assert.Equal("hello", Encoding.UTF8.GetString(captured.Value.Span));
        Assert.Equal(0, _registry.PendingCount);
    }

    // -------- End-to-end via async/await --------

    [Fact]
    public void EndToEnd_AwaitRpc_Succeeds()
    {
        async AtlasTask<int> AwaitReply(uint requestId)
        {
            var src = AtlasRpcSource<int>.Rent();
            src.Start(_registry, 100, requestId, DeserializeInt, 5000);
            return await src.Task;
        }

        var task = AwaitReply(123);
        // Driver: make sure pending entry exists, then dispatch.
        Assert.Equal(1, _registry.PendingCount);
        _registry.TryDispatch(100, 123, BuildIntReply(123, 777));
        Assert.Equal(777, _loop.RunAwait(task));
    }

    [Fact]
    public void EndToEnd_AwaitRpc_Cancelled_ThrowsAndCleansUp()
    {
        var cts = new AtlasCancellationSource();
        async AtlasTask<int> AwaitReply()
        {
            var src = AtlasRpcSource<int>.Rent();
            src.Start(_registry, 100, 999, DeserializeInt, 5000, cts.Token);
            return await src.Task;
        }

        var task = AwaitReply();
        Assert.Equal(1, _registry.PendingCount);
        cts.Cancel();
        Assert.Equal(0, _registry.PendingCount);
        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(task));
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
