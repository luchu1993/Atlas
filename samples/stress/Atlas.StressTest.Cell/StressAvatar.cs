using Atlas.DataTypes;
using Atlas.Entity;

namespace Atlas.StressTest.Cell;

// Cell-side StressAvatar. Echo is an own_client RPC — the client calls it
// and expects an EchoReply on itself to measure round-trip time. ReportPos
// is an all_clients RPC — the client streams position updates that we
// write into the position property so the AoI system broadcasts an
// EntityPositionUpdate envelope to in-range observers.
[Entity("StressAvatar")]
public partial class StressAvatar : ServerEntity
{
    public partial void Echo(uint seq, ulong clientTsNs)
    {
        // Reply straight back to the owning client. serverTsNs is captured
        // here so the client can decompose RTT into up-leg + down-leg.
        ulong serverTsNs = (ulong)(Time.ServerTime * 1_000_000_000.0);
        Client.EchoReply(seq, serverTsNs, clientTsNs);
    }

    public partial void ReportPos(Vector3 pos, Vector3 dir)
    {
        // Writing the property publishes a kEntityPositionUpdate envelope
        // to every observer currently tracking this entity. No validation
        // beyond what CellApp's AvatarUpdate path already applies
        // (isfinite, teleport bound) — stress test exercises volume, not
        // anti-cheat.
        Position = pos;
        _ = dir;  // reserved for facing replication once it's wired up
    }
}
