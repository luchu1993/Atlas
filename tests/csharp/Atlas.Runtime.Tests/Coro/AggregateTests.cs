using System;
using System.Threading;
using Atlas.Coro;
using Atlas.Coro.Testing;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("AtlasLoop")]
public sealed class AggregateTests : IDisposable
{
    private readonly TestLoop _loop;

    public AggregateTests()
    {
        _loop = new TestLoop();
        AtlasLoop.Install(_loop);
    }

    public void Dispose() => AtlasLoop.Reset();

    [Fact]
    public void WhenAll_Tuple2_BothComplete()
    {
        async AtlasTask<int> A() { await AtlasTask.Delay(50); return 10; }
        async AtlasTask<string> B() { await AtlasTask.Yield(); return "ok"; }

        var (a, b) = _loop.RunAwait(AtlasTask.WhenAll(A(), B()));
        Assert.Equal(10, a);
        Assert.Equal("ok", b);
    }

    [Fact]
    public void WhenAll_Tuple3_AllComplete()
    {
        async AtlasTask<int> A() { await AtlasTask.Yield(); return 1; }
        async AtlasTask<int> B() { await AtlasTask.Yield(); return 2; }
        async AtlasTask<int> C() { await AtlasTask.Yield(); return 3; }

        var (a, b, c) = _loop.RunAwait(AtlasTask.WhenAll(A(), B(), C()));
        Assert.Equal((1, 2, 3), (a, b, c));
    }

    [Fact]
    public void WhenAll_FirstFailure_Propagates()
    {
        async AtlasTask<int> Ok() { await AtlasTask.Yield(); return 7; }
        async AtlasTask<int> Bad()
        {
            await AtlasTask.Yield();
            throw new InvalidOperationException("bad");
        }

        Assert.Throws<InvalidOperationException>(
            () => _loop.RunAwait(AtlasTask.WhenAll(Ok(), Bad())));
    }

    [Fact]
    public void WhenAll_AllSyncCompleted_FastPath()
    {
        var result = _loop.RunAwait(AtlasTask.WhenAll(
            AtlasTask<int>.FromResult(1),
            AtlasTask<int>.FromResult(2)));
        Assert.Equal((1, 2), result);
    }

    [Fact]
    public void WhenAll_Array_ReturnsResults()
    {
        var tasks = new[]
        {
            DelayThenReturn(20, 100),
            DelayThenReturn(40, 200),
            DelayThenReturn(60, 300),
        };
        var arr = _loop.RunAwait(AtlasTask.WhenAll((System.Collections.Generic.IEnumerable<AtlasTask<int>>)tasks));
        Assert.Equal(new[] { 100, 200, 300 }, arr);
    }

    [Fact]
    public void WhenAll_EmptyEnumerable_CompletesSync()
    {
        var arr = _loop.RunAwait(AtlasTask.WhenAll(System.Linq.Enumerable.Empty<AtlasTask<int>>()));
        Assert.Empty(arr);
    }

    [Fact]
    public void WhenAll_VoidPair_CompletesAfterBoth()
    {
        async AtlasTask A() { await AtlasTask.Delay(20); }
        async AtlasTask B() { await AtlasTask.Delay(40); }

        _loop.RunAwait(AtlasTask.WhenAll(A(), B()));
    }

    [Fact]
    public void WhenAny_ReturnsFirstCompletedIndex()
    {
        async AtlasTask Slow() { await AtlasTask.Delay(1_000_000); }
        async AtlasTask Fast() { await AtlasTask.Delay(20); }

        var idx = _loop.RunAwait(AtlasTask.WhenAny(Slow(), Fast()));
        Assert.Equal(1, idx);
    }

    [Fact]
    public void WhenAny_TypedReturnsIndexAndResult()
    {
        async AtlasTask<int> Slow() { await AtlasTask.Delay(1_000_000); return 99; }
        async AtlasTask<int> Fast() { await AtlasTask.Delay(20); return 42; }

        var (idx, result) = _loop.RunAwait(AtlasTask.WhenAny(Slow(), Fast()));
        Assert.Equal(1, idx);
        Assert.Equal(42, result);
    }

    [Fact]
    public void WhenAny_FirstSyncCompleted_ReturnsImmediately()
    {
        async AtlasTask<int> SyncFirst() => 5;
        async AtlasTask<int> Slow() { await AtlasTask.Delay(1_000_000); return 99; }

        var (idx, result) = _loop.RunAwait(AtlasTask.WhenAny(SyncFirst(), Slow()));
        Assert.Equal(0, idx);
        Assert.Equal(5, result);
    }

    [Fact]
    public void FromBgWork_Generic_ReturnsValue()
    {
        var result = _loop.RunAwait(AtlasTask.FromBgWork(() => 42));
        Assert.Equal(42, result);
    }

    [Fact]
    public void FromBgWork_Action_RunsAndCompletes()
    {
        var ran = 0;
        _loop.RunAwait(AtlasTask.FromBgWork(() => Interlocked.Exchange(ref ran, 1)));
        Assert.Equal(1, ran);
    }

    [Fact]
    public void FromBgWork_Exception_Propagates()
    {
        var task = AtlasTask.FromBgWork<int>(() => throw new InvalidOperationException("worker"));
        Assert.Throws<InvalidOperationException>(() => _loop.RunAwait(task));
    }

    [Fact]
    public void FromBgWork_HoppingThreads()
    {
        // Worker captures a different thread id; final result observed on main.
        var workerThreadId = 0;
        var result = _loop.RunAwait(AtlasTask.FromBgWork(() =>
        {
            workerThreadId = Thread.CurrentThread.ManagedThreadId;
            return 7;
        }));
        Assert.Equal(7, result);
        Assert.NotEqual(0, workerThreadId);
        Assert.NotEqual(Thread.CurrentThread.ManagedThreadId, workerThreadId);
    }

    [Fact]
    public void WhenAll_MixedSyncAsync_BothObserved()
    {
        async AtlasTask<int> SyncOnly() => 11;
        async AtlasTask<int> AsyncDelay() { await AtlasTask.Yield(); return 13; }

        var (a, b) = _loop.RunAwait(AtlasTask.WhenAll(SyncOnly(), AsyncDelay()));
        Assert.Equal((11, 13), (a, b));
    }

    [Fact]
    public void WhenAll_SharedCancellation_AllChildrenAbort()
    {
        var cts = new AtlasCancellationSource();
        async AtlasTask Worker() => await AtlasTask.Delay(1_000_000, cts.Token);

        var task = AtlasTask.WhenAll(Worker(), Worker());
        _loop.RunNextFrame();
        cts.Cancel();
        Assert.Throws<OperationCanceledException>(() => _loop.RunAwait(task));
    }

    [Fact]
    public void WhenAny_AllSyncSameTime_PicksFirst()
    {
        var t1 = AtlasTask<int>.FromResult(100);
        var t2 = AtlasTask<int>.FromResult(200);
        var (idx, value) = _loop.RunAwait(AtlasTask.WhenAny(t1, t2));
        Assert.Equal(0, idx);
        Assert.Equal(100, value);
    }

    private static async AtlasTask<int> DelayThenReturn(int ms, int value)
    {
        await AtlasTask.Delay(ms);
        return value;
    }
}
