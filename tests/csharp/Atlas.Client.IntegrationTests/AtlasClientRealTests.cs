using System;
using System.Collections.Generic;
using System.Threading;
using Atlas.Client;
using Atlas.Client.Desktop;
using Atlas.Client.Native;
using Atlas.Coro;
using Atlas.Coro.Hosting;
using Atlas.Coro.Rpc;
using Xunit;

namespace Atlas.Client.IntegrationTests;

[Trait("Category", "Nightly")]
[Collection("RealCluster")]
public sealed class AtlasClientRealTests
{
    private readonly RealClusterFixture _cluster;

    public AtlasClientRealTests(RealClusterFixture cluster) { _cluster = cluster; }

    // Each test uses a unique username so LoginApp's pending_by_username_
    // dedup never rejects parallel / sequential cases.
    private static string FreshUser(string tag = "")
    {
        var s = $"int_{tag}{Guid.NewGuid():N}";
        return s.Length > 32 ? s.Substring(0, 32) : s;
    }

    private static void DriveUntil(AtlasClient client, Func<bool> pred, int timeoutMs = 30_000)
    {
        var deadline = DateTime.UtcNow.AddMilliseconds(timeoutMs);
        while (DateTime.UtcNow < deadline && !pred())
        {
            client.Update();
            Thread.Sleep(10);
        }
    }

    [Fact]
    public void ConnectAsync_AgainstRealCluster_ReachesConnected()
    {
        using var client = new AtlasClient();
        var task = client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, FreshUser(), "pwd-hash");
        DriveUntil(client, () => task.Status != AtlasTaskStatus.Pending);
        Assert.Equal(AtlasTaskStatus.Succeeded, task.Status);
        task.GetAwaiter().GetResult();
        Assert.Equal(AtlasNetState.Connected, client.State);
        Assert.NotNull(client.LastLogin);
        Assert.NotEqual(0u, client.LastAuth!.Value.EntityId);
    }

    [Fact]
    public void StateChanged_FiresMonotonicallyToConnected()
    {
        using var client = new AtlasClient();
        var seen = new List<AtlasNetState>();
        client.StateChanged += s => seen.Add(s);

        var task = client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, FreshUser(), "pwd-hash");
        DriveUntil(client, () => task.Status != AtlasTaskStatus.Pending);
        Assert.Equal(AtlasTaskStatus.Succeeded, task.Status);

        // Update polls state on its own cadence; consecutive transitions can
        // collapse, so assert monotonic progression rather than exact list.
        Assert.Contains(AtlasNetState.Connected, seen);
        var last = AtlasNetState.Disconnected;
        foreach (var s in seen)
        {
            Assert.True((int)s >= (int)last,
                $"state regressed: {last} -> {s}; full sequence: [{string.Join(",", seen)}]");
            last = s;
        }
    }

    [Fact]
    public void Disconnect_AfterConnect_StateReturnsToDisconnected()
    {
        using var client = new AtlasClient();
        var task = client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, FreshUser(), "pwd-hash");
        DriveUntil(client, () => task.Status != AtlasTaskStatus.Pending);
        Assert.Equal(AtlasNetState.Connected, client.State);

        client.Disconnect();
        DriveUntil(client, () => client.State == AtlasNetState.Disconnected, timeoutMs: 5_000);
        Assert.Equal(AtlasNetState.Disconnected, client.State);
    }

    [Fact]
    public void Reconnect_OnSameClient_AfterDisconnect_Succeeds()
    {
        using var client = new AtlasClient();
        var user = FreshUser();
        var first = client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, user, "pwd-hash");
        DriveUntil(client, () => first.Status != AtlasTaskStatus.Pending);
        Assert.Equal(AtlasTaskStatus.Succeeded, first.Status);

        client.Disconnect();
        DriveUntil(client, () => client.State == AtlasNetState.Disconnected, timeoutMs: 5_000);

        var second = client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, user, "pwd-hash");
        DriveUntil(client, () => second.Status != AtlasTaskStatus.Pending);
        Assert.Equal(AtlasTaskStatus.Succeeded, second.Status);
        Assert.Equal(AtlasNetState.Connected, client.State);
    }

    [Fact]
    public void ConcurrentClients_DifferentUsers_AllReachConnected()
    {
        // Two clients share one coro loop (AtlasLoop.Install is global).
        var loop = new ManagedAtlasLoop();
        AtlasLoop.Install(loop);
        var registry = new ManagedRpcRegistry(loop);
        AtlasRpcRegistryHost.Install(registry);
        try
        {
            using var a = new AtlasClient(installCoroLoop: false);
            using var b = new AtlasClient(installCoroLoop: false);

            var ta = a.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, FreshUser("a"), "pwd-hash");
            var tb = b.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, FreshUser("b"), "pwd-hash");

            var deadline = DateTime.UtcNow.AddSeconds(30);
            while (DateTime.UtcNow < deadline &&
                   (ta.Status == AtlasTaskStatus.Pending || tb.Status == AtlasTaskStatus.Pending))
            {
                a.Update();
                b.Update();
                loop.Drain();
                Thread.Sleep(10);
            }

            Assert.Equal(AtlasTaskStatus.Succeeded, ta.Status);
            Assert.Equal(AtlasTaskStatus.Succeeded, tb.Status);
            Assert.NotEqual(a.LastAuth!.Value.EntityId, b.LastAuth!.Value.EntityId);
        }
        finally
        {
            AtlasRpcRegistryHost.Reset();
            registry.Dispose();
            loop.Dispose();
            AtlasLoop.Reset();
        }
    }
}

[CollectionDefinition("FakeCluster")]
public sealed class FakeClusterCollection : ICollectionFixture<FakeClusterFixture> { }

[CollectionDefinition("RealCluster")]
public sealed class RealClusterCollection : ICollectionFixture<RealClusterFixture> { }
