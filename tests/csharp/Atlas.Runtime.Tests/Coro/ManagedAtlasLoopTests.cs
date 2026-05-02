using System;
using System.Threading;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Xunit;

namespace Atlas.Tests.Coro;

[Collection("AtlasLoop")]
public sealed class ManagedAtlasLoopTests : IDisposable
{
    private readonly ManagedAtlasLoop _loop;

    public ManagedAtlasLoopTests()
    {
        _loop = new ManagedAtlasLoop();
        AtlasLoop.Install(_loop);
    }

    public void Dispose()
    {
        AtlasLoop.Reset();
        _loop.Dispose();
    }

    [Fact]
    public void IsMainThread_TrueOnConstructionThread()
    {
        Assert.True(_loop.IsMainThread);
    }

    [Fact]
    public void PostMainThread_DefersUntilDrain()
    {
        var ran = 0;
        _loop.PostMainThread(_ => Interlocked.Exchange(ref ran, 1), null);
        Assert.Equal(0, ran);
        Assert.Equal(1, _loop.PendingCount);
        _loop.Drain();
        Assert.Equal(1, ran);
        Assert.Equal(0, _loop.PendingCount);
    }

    [Fact]
    public void Drain_IncrementsCurrentFrame()
    {
        var f0 = _loop.CurrentFrame;
        _loop.Drain();
        Assert.Equal(f0 + 1, _loop.CurrentFrame);
    }

    [Fact]
    public void Timer_EnqueuesOnFireAndRunsOnDrain()
    {
        var done = new ManualResetEventSlim(false);
        _loop.RegisterTimer(20, _ => done.Set(), null);

        var deadline = Environment.TickCount64 + 1000;
        while (Environment.TickCount64 < deadline && _loop.PendingCount == 0)
            Thread.Sleep(1);

        Assert.True(_loop.PendingCount >= 1, "timer should have queued main-thread callback");
        _loop.Drain();
        Assert.True(done.IsSet);
        Assert.Equal(0, _loop.ActiveTimers);
    }

    [Fact]
    public void CancelTimer_BeforeFire_SkipsCallback()
    {
        var ran = 0;
        var handle = _loop.RegisterTimer(10_000,
            _ => Interlocked.Exchange(ref ran, 1), null);
        _loop.CancelTimer(handle);
        Thread.Sleep(20);
        _loop.Drain();
        Assert.Equal(0, ran);
        Assert.Equal(0, _loop.ActiveTimers);
    }

    [Fact]
    public void AtlasTaskDelay_ViaManagedAtlasLoop_Completes()
    {
        async AtlasTask<int> Body()
        {
            await AtlasTask.Delay(20);
            return 42;
        }

        var task = Body();
        var awaiter = task.GetAwaiter();
        var deadline = Environment.TickCount64 + 2000;
        while (Environment.TickCount64 < deadline && !awaiter.IsCompleted)
        {
            _loop.Drain();
            Thread.Sleep(1);
        }
        Assert.True(awaiter.IsCompleted);
        Assert.Equal(42, awaiter.GetResult());
    }

    [Fact]
    public void FromBgWork_ViaManagedAtlasLoop_PostsBackToMain()
    {
        var workerThread = 0;
        async AtlasTask<int> Body()
        {
            return await AtlasTask.FromBgWork(() =>
            {
                workerThread = Thread.CurrentThread.ManagedThreadId;
                return 7;
            });
        }

        var task = Body();
        var awaiter = task.GetAwaiter();
        var deadline = Environment.TickCount64 + 2000;
        while (Environment.TickCount64 < deadline && !awaiter.IsCompleted)
        {
            _loop.Drain();
            Thread.Sleep(1);
        }
        Assert.True(awaiter.IsCompleted);
        Assert.Equal(7, awaiter.GetResult());
        Assert.NotEqual(0, workerThread);
        Assert.NotEqual(Thread.CurrentThread.ManagedThreadId, workerThread);
    }
}
