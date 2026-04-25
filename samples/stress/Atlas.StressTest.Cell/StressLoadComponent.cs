using System.Collections.Generic;
using Atlas.Def;

namespace Atlas.Components;

// Cell-side user partial for StressLoadComponent. Codegen emits the
// class declaration + property setters + RPC partials in the
// Atlas.Components namespace; this file fills in the receive-side
// method bodies so the dispatcher's call lands somewhere with effect.
//
// Send-side stubs (OnAffixesUpdated for client_methods) need no user
// body — codegen generates the SendClientRpc-shaped implementation.
public sealed partial class StressLoadComponent
{
    // Charge — scalar args. Bumps extraHp so the next frame's component
    // delta carries the changed scalar; the seq is currently unused but
    // mirrors Echo's RTT-trace pattern in case world_stress wants to
    // measure component-RPC latency separately later.
    public partial void Charge(uint seq, int amount)
    {
        ExtraHp = ExtraHp + amount;
        _ = seq;
    }

    // ApplyAffixes — list arg. Replace the synced list contents so a
    // single inbound RPC produces a clear+append op-log frame.
    public partial void ApplyAffixes(List<int> ids)
    {
        Affixes.Clear();
        foreach (var id in ids) Affixes.Add(id);
    }

    // SwapOffhand — struct arg. Field-level struct sync activates on
    // the wholesale assignment.
    public partial void SwapOffhand(StressWeapon w)
    {
        Offhand = w;
    }

    // QueueBuffs — list-of-struct arg.
    public partial void QueueBuffs(List<StressBuff> buffs)
    {
        foreach (var b in buffs) QuickBuffs.Add(b);
    }
}
