using System;
using System.Threading;
using Atlas.Client;
using Atlas.Client.Desktop;
using Atlas.Client.IntegrationTests.Native;
using Atlas.Client.Native;
using Atlas.Coro;
using Xunit;

namespace Atlas.Client.IntegrationTests;

[Collection("FakeCluster")]
public sealed class AtlasClientFakeTests : IDisposable
{
    private readonly FakeClusterFixture _cluster;
    private readonly AtlasClient _client;

    public AtlasClientFakeTests(FakeClusterFixture cluster)
    {
        _cluster = cluster;
        _cluster.SetLoginPolicy(FakeLoginPolicy.Accept);
        _cluster.SetAuthPolicy(FakeAuthPolicy.Accept);
        _client = new AtlasClient();
    }

    public void Dispose() => _client.Dispose();

    private void DriveUntil(Func<bool> pred, int timeoutMs = 5000)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline && !pred())
        {
            _cluster.Pump(20);
            _client.Update();
            Thread.Sleep(2);
        }
    }

    [Fact]
    public void ConnectAsync_HappyPath_ReachesConnectedState()
    {
        var task = _client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, "alice", "pwd-hash");
        DriveUntil(() => task.Status != AtlasTaskStatus.Pending, timeoutMs: 8000);
        var awaiter = task.GetAwaiter();
        Assert.Equal(AtlasTaskStatus.Succeeded, task.Status);
        awaiter.GetResult();
        Assert.Equal(AtlasNetState.Connected, _client.State);
        Assert.NotNull(_client.LastLogin);
        Assert.Equal((uint)42, _client.LastAuth!.Value.EntityId);
        Assert.Equal((ushort)7, _client.LastAuth!.Value.TypeId);
    }

    [Fact]
    public void LoginAsync_RejectInvalidCreds_ThrowsLoginFailedException()
    {
        _cluster.SetLoginPolicy(FakeLoginPolicy.RejectInvalidCreds);
        var task = _client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, "alice", "bad");
        DriveUntil(() => task.Status != AtlasTaskStatus.Pending, timeoutMs: 5000);
        Assert.Equal(AtlasTaskStatus.Faulted, task.Status);
        var ex = Assert.Throws<LoginFailedException>(() => task.GetAwaiter().GetResult());
        Assert.Equal(AtlasLoginStatus.InvalidCredentials, ex.Status);
    }

    [Fact]
    public void AuthenticateAsync_Reject_ThrowsAuthFailedException()
    {
        _cluster.SetAuthPolicy(FakeAuthPolicy.Reject);
        var task = _client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, "alice", "pwd-hash");
        DriveUntil(() => task.Status != AtlasTaskStatus.Pending, timeoutMs: 8000);
        Assert.Equal(AtlasTaskStatus.Faulted, task.Status);
        Assert.Throws<AuthFailedException>(() => task.GetAwaiter().GetResult());
    }

    [Fact]
    public void ConnectAsync_CtCancelled_BeforeReply_ResultIsCanceled()
    {
        _cluster.SetLoginPolicy(FakeLoginPolicy.NeverReply);
        var cts = new AtlasCancellationSource();
        var task = _client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, "alice", "pwd-hash", cts.Token);
        // Give the request time to leave the wire.
        DriveUntil(() => _cluster.LoginRequestSeen, timeoutMs: 3000);
        cts.Cancel();
        DriveUntil(() => task.Status != AtlasTaskStatus.Pending, timeoutMs: 2000);
        Assert.Equal(AtlasTaskStatus.Canceled, task.Status);
        Assert.Throws<OperationCanceledException>(() => task.GetAwaiter().GetResult());
    }

    [Fact]
    public void Dispose_DuringInflightLogin_StrandsAwaiterAsCanceled()
    {
        _cluster.SetLoginPolicy(FakeLoginPolicy.NeverReply);
        var task = _client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, "alice", "pwd-hash");
        DriveUntil(() => _cluster.LoginRequestSeen, timeoutMs: 3000);
        _client.Dispose();
        Assert.Equal(AtlasTaskStatus.Canceled, task.Status);
    }
}
