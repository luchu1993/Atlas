using System;
using System.Collections.Generic;
using Atlas.Client;
using Atlas.Components;
using Atlas.DataTypes;
using Atlas.Def;
using Atlas.Diagnostics;

namespace Atlas.ClientSample;

// `[StressAvatar:<id>] <event> key=value …` log shape is parsed by
// world_stress client_event_tap; keep the prefix stable.
[Atlas.Entity.Entity("StressAvatar")]
public partial class StressAvatar : ClientEntity
{
    protected override void OnInit()
    {
        Log.Info($"[StressAvatar:{EntityId}] OnInit");
    }

    protected override void OnEnterWorld()
    {
        Log.Info(
            $"[StressAvatar:{EntityId}] OnEnterWorld pos={FormatVec(Position)} hp={Hp}");
    }

    protected override void OnDestroy()
    {
        Log.Info($"[StressAvatar:{EntityId}] OnDestroy");
    }

    partial void OnHpChanged(int oldValue, int newValue)
    {
        Log.Info(
            $"[StressAvatar:{EntityId}] OnHpChanged old={oldValue} new={newValue}");
    }

    partial void OnMainWeaponChanged(Atlas.Def.StressWeapon oldValue, Atlas.Def.StressWeapon newValue)
    {
        Log.Info(
            $"[StressAvatar:{EntityId}] OnMainWeaponChanged old.id={oldValue.Id} new.id={newValue.Id}");
    }

    protected override void OnPositionUpdated(Vector3 newPos)
    {
        // Peers show samples>0 + lat>0 (filter-driven); owner shows samples=-1
        // + lat=0.000 (snap path skips AvatarFilter).
        int samples = Filter?.SampleCount ?? -1;
        double lat = Filter?.CurrentLatency ?? 0.0;
        Log.Info(
            $"[StressAvatar:{EntityId}] OnPositionUpdated pos={FormatVec(newPos)} samples={samples} lat={lat:F3}");
    }

    public partial void EchoReply(uint seq, ulong serverTsNs, ulong clientTsNs)
    {
        // Real stress measurement is done by world_stress virtual clients
        // parsing the raw wire; the script path just confirms the RPC lands.
        Log.Info(
            $"[StressAvatar:{EntityId}] EchoReply seq={seq} serverTsNs={serverTsNs} clientTsNs={clientTsNs}");
        var m = Metrics;
        if (m != null) m.EchoReplyCount++;
    }

    public partial void OnWeaponBroken(StressWeapon w)
    {
        Log.Info(
            $"[StressAvatar:{EntityId}] OnWeaponBroken id={w.Id} sharpness={w.Sharpness}");
        var m = Metrics;
        if (m != null) m.OnWeaponBrokenCount++;
    }

    // Server→client list push.
    public partial void OnScoresSnapshot(List<int> scores)
    {
        Log.Info(
            $"[StressAvatar:{EntityId}] OnScoresSnapshot count={scores.Count}");
        var m = Metrics;
        if (m != null) m.OnScoresSnapshotCount++;
    }

    // Broadcast (RpcTarget.All) RPC — every client witnessing this
    // avatar logs receipt, including the owner.
    public partial void OnAreaBroadcast(uint seq, int payload)
    {
        Log.Info(
            $"[StressAvatar:{EntityId}] OnAreaBroadcast seq={seq} payload={payload}");
        var m = Metrics;
        if (m != null) m.OnAreaBroadcastCount++;
    }

    private static string FormatVec(Vector3 v) => $"({v.X:F2},{v.Y:F2},{v.Z:F2})";
}
