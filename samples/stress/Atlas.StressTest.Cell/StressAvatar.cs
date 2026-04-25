using System.Collections.Generic;
using Atlas.Components;
using Atlas.DataTypes;
using Atlas.Def;
using Atlas.Entity;

namespace Atlas.StressTest.Cell;

// Cell-side StressAvatar.
//
// All per-tick mutation lives on StressTimerComponent (ServerLocal); the
// entity body now hosts only RPC implementations + lifecycle. Synced
// state still flows through the auto-generated property setters, which
// the timer component drives through the typed accessors.
[Entity("StressAvatar")]
public partial class StressAvatar : ServerEntity
{
    protected override void OnInit(bool isReload)
    {
        if (isReload) return;

        // Synced + ServerLocal components attach in OnInit so the
        // replication pump's first frame already sees the slot table.
        // The Synced component's initial values land in the entity-enter
        // snapshot — until a dedicated full-snapshot path lands, the first
        // delta after any field mutation carries them.
        AddComponent<StressLoadComponent>();
        AddLocalComponent<StressTimerComponent>();
    }

    // ------ Existing scalar RPCs --------------------------------------------

    public partial void Echo(uint seq, ulong clientTsNs)
    {
        ulong serverTsNs = (ulong)(Time.ServerTime * 1_000_000_000.0);
        Client.EchoReply(seq, serverTsNs, clientTsNs);
    }

    public partial void ReportPos(Vector3 pos, Vector3 dir)
    {
        Position = pos;
        _ = dir;  // facing replication is reserved.
    }

    // ------ Container / struct RPC implementations -------------------------

    // list<int32> arg — receive the batch and append into the synced
    // scores list so the wire delta on the next tick reflects it.
    public partial void SubmitScores(List<int> batch)
    {
        foreach (var s in batch) Scores.Add(s);
    }

    // string + list<int32> mixed args — write into the dict-of-list
    // property. The inner ObservableList must be constructed explicitly
    // because the dict's value type is the observable wrapper, not a
    // plain List<T>.
    public partial void UpdateLoadout(string name, List<int> items)
    {
        var inner = new global::Atlas.Observable.ObservableList<int>();
        foreach (var x in items) inner.Add(x);
        Loadouts[name] = inner;
    }

    // struct arg — entity struct properties use the MutRef pattern, so
    // assignment goes field-by-field. Each setter trips the field-level
    // dirty bit; the wire output is the same 7-byte struct body either
    // way (whole-sync vs field-sync chosen by the auto rule on size).
    public partial void EquipWeapon(StressWeapon w)
    {
        MainWeapon.Id        = w.Id;
        MainWeapon.Sharpness = w.Sharpness;
        MainWeapon.Bound     = w.Bound;
    }

    // list-of-struct arg — append each into the synced buffs list so
    // each StressBuff lands in the op-log frame.
    public partial void ApplyBuffs(List<StressBuff> buffs)
    {
        foreach (var b in buffs) Buffs.Add(b);
    }
}
