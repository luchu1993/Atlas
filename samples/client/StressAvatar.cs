using System;
using Atlas.Client;
using Atlas.DataTypes;

namespace Atlas.ClientSample;

// Client-side StressAvatar: the entity the stress cluster creates via
// Account.SelectAvatar. world_stress `--script-clients N` launches real
// atlas_client.exe subprocesses that load this assembly; every breadcrumb
// here is grepped against by the Phase C2 harness to verify the Phase B
// callback contract end-to-end.
//
// The log line format (the `[StressAvatar:<id>] <event> key=value …` shape)
// is a test contract — keep it parse-stable unless updating
// client_event_tap.* in world_stress at the same time.
[Atlas.Entity.Entity("StressAvatar")]
public partial class StressAvatar : ClientEntity
{
    // -------- Lifecycle (A / B3) --------------------------------------------

    protected override void OnInit()
    {
        ClientLog.Info($"[StressAvatar:{EntityId}] OnInit");
    }

    protected override void OnEnterWorld()
    {
        ClientLog.Info(
            $"[StressAvatar:{EntityId}] OnEnterWorld pos={FormatVec(Position)} hp={Hp}");
    }

    protected override void OnDestroy()
    {
        ClientLog.Info($"[StressAvatar:{EntityId}] OnDestroy");
    }

    // -------- Property-change callback (B1, wire-delta-only per B2) --------

    partial void OnHpChanged(int oldValue, int newValue)
    {
        ClientLog.Info(
            $"[StressAvatar:{EntityId}] OnHpChanged old={oldValue} new={newValue}");
    }

    // -------- Transform (B4) -------------------------------------------------

    protected override void OnPositionUpdated(Vector3 newPos)
    {
        ClientLog.Info(
            $"[StressAvatar:{EntityId}] OnPositionUpdated pos={FormatVec(newPos)}");
    }

    // -------- RPC receiver (pre-existing) -----------------------------------

    public partial void EchoReply(uint seq, ulong serverTsNs, ulong clientTsNs)
    {
        // Real stress measurement is done by world_stress virtual clients
        // parsing the raw wire; the script path just confirms the RPC lands.
        ClientLog.Info(
            $"[StressAvatar:{EntityId}] EchoReply seq={seq} serverTsNs={serverTsNs} clientTsNs={clientTsNs}");
    }

    private static string FormatVec(Vector3 v) => $"({v.X:F2},{v.Y:F2},{v.Z:F2})";
}
