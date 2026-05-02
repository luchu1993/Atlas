using System;
using Atlas.Coro;
using Atlas.Coro.Testing;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("AtlasLoop")]
public sealed class AtlasTaskTests : IDisposable
{
    private readonly TestLoop _loop;

    public AtlasTaskTests()
    {
        _loop = new TestLoop();
        AtlasLoop.Install(_loop);
    }

    public void Dispose() => AtlasLoop.Reset();

    [Fact]
    public void Sync_AsyncMethodReturnsValue()
    {
        Assert.Equal(42, _loop.RunAwait(ReturnFortyTwo()));
    }

    private static async AtlasTask<int> ReturnFortyTwo() => 42;

    [Fact]
    public void Sync_AsyncMethodPropagatesException()
    {
        Assert.Throws<InvalidOperationException>(() => _loop.RunAwait(ThrowSync()));
    }

    private static async AtlasTask<int> ThrowSync()
    {
        throw new InvalidOperationException("boom");
    }

    [Fact]
    public void Yield_ResumesAfterOneFrame()
    {
        var executed = false;
        async AtlasTask Body()
        {
            await AtlasTask.Yield();
            executed = true;
        }
        _loop.RunAwait(Body());
        Assert.True(executed);
    }

    [Fact]
    public void Delay_ResumesAfterTimeout()
    {
        async AtlasTask<int> Body()
        {
            await AtlasTask.Delay(100);
            return 7;
        }
        Assert.Equal(7, _loop.RunAwait(Body()));
    }

    [Fact]
    public void Delay_CancelledMidFlight_Throws()
    {
        var cts = new AtlasCancellationSource();
        async AtlasTask Body() => await AtlasTask.Delay(1_000_000, cts.Token);

        var task = Body();
        _loop.RunNextFrame();   // give the state machine one tick to suspend on the timer
        cts.Cancel();

        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(task));
    }

    [Fact]
    public void Delay_AlreadyCancelled_Throws()
    {
        var cts = new AtlasCancellationSource();
        cts.Cancel();
        async AtlasTask Body() => await AtlasTask.Delay(50, cts.Token);

        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(Body()));
    }

    [Fact]
    public void WaitUntil_PredicateBecomesTrue()
    {
        var counter = new Counter();
        async AtlasTask<int> Body()
        {
            await AtlasTask.WaitUntil(static c => ((Counter)c!).Value >= 3, counter);
            return counter.Value;
        }

        var task = Body();
        for (var i = 0; i < 5; i++)
        {
            counter.Value++;
            _loop.RunNextFrame();
        }
        Assert.True(_loop.RunAwait(task) >= 3);
    }

    [Fact]
    public void WaitUntil_AlreadyTrue_ShortCircuits()
    {
        async AtlasTask Body() => await AtlasTask.WaitUntil(static () => true);
        _loop.RunAwait(Body(), maxFrames: 2);
    }

    [Fact]
    public void WaitUntil_Cancelled_Throws()
    {
        var cts = new AtlasCancellationSource();
        async AtlasTask Body()
            => await AtlasTask.WaitUntil(static () => false, cts.Token);

        var task = Body();
        _loop.RunNextFrame();
        cts.Cancel();
        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(task));
    }

    [Fact]
    public void Cancellation_LinkedSourcePropagates()
    {
        var parent = new AtlasCancellationSource();
        var child = parent.CreateLinked();
        Assert.False(child.IsCancellationRequested);
        parent.Cancel();
        Assert.True(child.IsCancellationRequested);
    }

    [Fact]
    public void Cancellation_LinkedSourceBornCancelled()
    {
        var parent = new AtlasCancellationSource();
        parent.Cancel();
        var child = parent.CreateLinked();
        Assert.True(child.IsCancellationRequested);
    }

    [Fact]
    public void Cancellation_RegisterFiresOnCancel()
    {
        var src = new AtlasCancellationSource();
        var flag = new Counter();
        using var _ = src.Token.Register(static c => ((Counter)c!).Value = 1, flag);
        Assert.Equal(0, flag.Value);
        src.Cancel();
        Assert.Equal(1, flag.Value);
    }

    [Fact]
    public void Cancellation_RegisterAfterCancelFiresInline()
    {
        var src = new AtlasCancellationSource();
        src.Cancel();
        var flag = new Counter();
        var reg = src.Token.Register(static c => ((Counter)c!).Value = 1, flag);
        Assert.Equal(1, flag.Value);
        reg.Dispose();
    }

    [Fact]
    public void Cancellation_DisposeUnregistersCallback()
    {
        var src = new AtlasCancellationSource();
        var flag = new Counter();
        var reg = src.Token.Register(static c => ((Counter)c!).Value = 1, flag);
        reg.Dispose();
        src.Cancel();
        Assert.Equal(0, flag.Value);
    }

    [Fact]
    public void ThrowIfCancellationRequested_AfterCancel_Throws()
    {
        var src = new AtlasCancellationSource();
        src.Cancel();
        Assert.Throws<OperationCanceledException>(() => src.Token.ThrowIfCancellationRequested());
    }

    [Fact]
    public void ThrowIfCancellationRequested_NoneToken_NoThrow()
    {
        AtlasCancellationToken.None.ThrowIfCancellationRequested();
    }

    [Fact]
    public void ChainedAwaits_ProduceCorrectValue()
    {
        async AtlasTask<int> Inner(int x)
        {
            await AtlasTask.Yield();
            return x * 2;
        }
        async AtlasTask<int> Outer()
        {
            var a = await Inner(3);
            var b = await Inner(5);
            return a + b;
        }
        Assert.Equal(16, _loop.RunAwait(Outer()));
    }

    [Fact]
    public void FromResult_AwaitReturnsValue()
    {
        async AtlasTask<int> Body() => await AtlasTask<int>.FromResult(99);
        Assert.Equal(99, _loop.RunAwait(Body()));
    }

    [Fact]
    public void FromException_AwaitThrows()
    {
        async AtlasTask<int> Body() => await AtlasTask<int>.FromException(new ArgumentException("expected"));
        Assert.Throws<ArgumentException>(() => _loop.RunAwait(Body()));
    }

    [Fact]
    public void CompletedTask_IsImmediatelyDone()
    {
        async AtlasTask Body() => await AtlasTask.CompletedTask;
        _loop.RunAwait(Body());
    }

    [Fact]
    public void AtlasResult_OkAndError()
    {
        AtlasResult<int, string> ok = 42;
        AtlasResult<int, string> err = "oops";
        Assert.True(ok.IsOk);
        Assert.False(ok.IsError);
        Assert.Equal(42, ok.Value);
        Assert.True(err.IsError);
        Assert.Equal("oops", err.Error);
        Assert.True(ok.TryGetValue(out var v) && v == 42);
        Assert.True(err.TryGetError(out var e) && e == "oops");
    }

    [Fact]
    public void DefaultLoop_ThrowsOnUse()
    {
        AtlasLoop.Reset();
        Assert.Throws<InvalidOperationException>(() => AtlasLoop.Current.PostNextFrame((_) => { }, null));
        AtlasLoop.Install(_loop);     // restore for Dispose
    }

    private sealed class Counter
    {
        public int Value;
    }
}
