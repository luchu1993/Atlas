using System;
using System.Linq;
using System.Threading;
using Atlas.Client;
using Atlas.Client.Desktop;
using Atlas.Client.IntegrationTests.Native;
using Atlas.Client.Native;
using Atlas.Coro;
using Atlas.DataTypes;
using Xunit;

namespace Atlas.Client.IntegrationTests;

// End-to-end coverage for the AvatarFilter wire path through the production
// atlas_net_client.dll: FakeCluster injects 0xF001 / 0xF003 envelopes, which
// flow through net_client's on_deliver -> AtlasNetCallbackBridge ->
// IAtlasNetEvents.OnDeliver -> ClientCallbacks.DeliverFromServer ->
// ClientEntityManager.OnEnter / ApplyPosition -> ClientEntity.Filter.
[Collection("FakeCluster")]
public sealed class AvatarFilterFakeClusterTests : IDisposable
{
    private readonly FakeClusterFixture _cluster;
    private readonly AtlasClient _client;

    private sealed class FakePeer : ClientEntity
    {
        public override string TypeName => "FakePeer";
        public override ushort TypeId => 999;
    }

    public AvatarFilterFakeClusterTests(FakeClusterFixture cluster)
    {
        _cluster = cluster;
        _cluster.SetLoginPolicy(FakeLoginPolicy.Accept);
        _cluster.SetAuthPolicy(FakeAuthPolicy.Accept);
        _client = new AtlasClient();

        ClientEntityFactory.Register(999, () => new FakePeer());
        // Static EntityManager carries entries between tests in the same xunit
        // collection; clear before each fact so peer-id reuse is safe.
        var mgr = ClientCallbacks.EntityManager;
        foreach (var e in mgr.Entities.ToList()) mgr.Destroy(e.EntityId);
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

    private void ConnectAndAuth()
    {
        var task = _client.ConnectAsync("127.0.0.1", _cluster.LoginAppPort, "alice", "pwd-hash");
        DriveUntil(() => task.Status != AtlasTaskStatus.Pending, timeoutMs: 8000);
        Assert.Equal(AtlasTaskStatus.Succeeded, task.Status);
        task.GetAwaiter().GetResult();
        Assert.Equal(AtlasNetState.Connected, _client.State);
    }

    [Fact]
    public void EntityEnterEnvelopeCreatesPeerWithFilter()
    {
        ConnectAndAuth();

        const uint kPeerEid = 1234;
        Assert.True(_cluster.PushEntityEnter(eid: kPeerEid, typeId: 999,
            px: 0, py: 0, pz: 0, dx: 0, dy: 0, dz: 1,
            onGround: true, serverTime: 100.0));

        ClientEntity? peer = null;
        DriveUntil(() => (peer = ClientCallbacks.EntityManager.Get(kPeerEid)) != null);

        Assert.NotNull(peer);
        Assert.False(peer!.IsOwner);
        Assert.NotNull(peer.Filter);
        Assert.Equal(1, peer.Filter!.SampleCount);
        Assert.Equal(100.0, peer.LastPositionServerTime);
    }

    [Fact]
    public void VolatilePositionUpdateGrowsFilterRing()
    {
        ConnectAndAuth();

        const uint kPeerEid = 5678;
        Assert.True(_cluster.PushEntityEnter(eid: kPeerEid, typeId: 999,
            px: 0, py: 0, pz: 0, dx: 0, dy: 0, dz: 1,
            onGround: true, serverTime: 200.0));

        ClientEntity? peer = null;
        DriveUntil(() => (peer = ClientCallbacks.EntityManager.Get(kPeerEid)) != null);
        Assert.NotNull(peer);

        // Volatile rides 0xF001 (unreliable); send a small burst and assert the
        // ring fills monotonically. Loopback can drop a packet so the exact
        // count varies, but at least one of the three should land.
        const int kBurst = 3;
        for (int i = 1; i <= kBurst; ++i)
        {
            Assert.True(_cluster.PushEntityPositionUpdate(eid: kPeerEid,
                px: 10f * i, py: 0, pz: 0, dx: 0, dy: 0, dz: 1,
                onGround: true, serverTime: 200.0 + 0.1 * i));
        }
        DriveUntil(() => peer!.Filter!.SampleCount >= 2, timeoutMs: 3000);
        Assert.True(peer!.Filter!.SampleCount >= 2,
            $"expected ≥2 samples (1 enter + ≥1 volatile), got {peer.Filter!.SampleCount}");
        Assert.True(peer.LastPositionServerTime > 200.0,
            $"serverTime should have advanced past enter; got {peer.LastPositionServerTime}");
    }
}
