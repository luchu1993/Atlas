using Atlas.Components;

namespace Atlas.Components;

// ClientLocal — pure client-side measurement component. Holds RTT
// samples and observation counters for the script harness; no protocol
// surface. Hand-written, lives in Atlas.Components so the codegen-
// emitted Metrics accessor on StressAvatar resolves.
public sealed class StressMetricsComponent : ClientLocalComponent
{
    public int EchoReplyCount;
    public int OnHpChangedCount;
    public int OnWeaponBrokenCount;
    public int OnScoresSnapshotCount;
}
