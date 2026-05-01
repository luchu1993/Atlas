using Atlas.Def;
using Atlas.Entity.Components;
using Atlas.StressTest.Cell;

// Lives in Atlas.Components because EntityComponentAccessorEmitter looks
// up local components there (the same namespace codegen-emitted Synced
// components live in). Hand-written ServerLocal classes follow the same
// rule even though no Atlas.Components codegen runs for them.
namespace Atlas.Components;

// ServerLocal component that owns every per-tick mutator on the Stress
// avatar. Pulled out of StressAvatar so the entity body stays focused on
// RPC implementations + component lifecycle, and so the stress harness
// can swap timer pacing without touching the .def-driven state surface.
//
// All cadences picked so the wire-level baseline comparison can attribute
// bytes/s deltas cleanly: each mutation type fires at a distinct period.
public sealed class StressTimerComponent : ServerLocalComponent
{
    private const int   kHpInitial          = 100;
    private const float kHpPeriod           = 1.0f;
    private const float kWeaponPeriod       = 1.0f;
    private const float kScoresPeriod       = 1.0f;
    private const int   kScoresClearEvery   = 10;
    private const float kResistsPeriod      = 2.0f;
    private const int   kResistsClearEvery  = 10;
    private const float kCombosPeriod       = 5.0f;
    private const int   kCombosClearEvery   = 6;
    private const float kBuffsPeriod        = 3.0f;
    private const int   kBuffsClearEvery    = 7;
    private const float kSpellSlotsPeriod   = 4.0f;
    private const float kLoadoutsPeriod     = 6.0f;

    private float _hpAccum, _weaponAccum, _scoresAccum, _resistsAccum;
    private float _combosAccum, _buffsAccum, _spellSlotsAccum, _loadoutsAccum;
    private int _scoresTickCount, _resistsTickCount, _combosTickCount, _buffsTickCount;
    private int _spellSlotsTickCount, _loadoutsTickCount;

    public override void OnAttached()
    {
        var avatar = (StressAvatar)Entity;
        // Initial state mirrors the pre-component baseline so existing
        // golden expectations (Hp=100, MainWeapon.Id=1000) still hold.
        avatar.Hp = kHpInitial;
        avatar.MainWeapon.Id = 1000;
        avatar.MainWeapon.Sharpness = 100;
        avatar.MainWeapon.Bound = false;
    }

    public override void OnTick(float dt)
    {
        var a = (StressAvatar)Entity;

        _hpAccum += dt;
        while (_hpAccum >= kHpPeriod)
        {
            _hpAccum -= kHpPeriod;
            a.Hp = a.Hp > 1 ? a.Hp - 1 : kHpInitial;
        }

        _weaponAccum += dt;
        while (_weaponAccum >= kWeaponPeriod)
        {
            _weaponAccum -= kWeaponPeriod;
            a.MainWeapon.Id = a.MainWeapon.Id + 1;
        }

        _scoresAccum += dt;
        while (_scoresAccum >= kScoresPeriod)
        {
            _scoresAccum -= kScoresPeriod;
            _scoresTickCount++;
            if (_scoresTickCount % kScoresClearEvery == 0) a.Scores.Clear();
            a.Scores.Add(_scoresTickCount);
        }

        _resistsAccum += dt;
        while (_resistsAccum >= kResistsPeriod)
        {
            _resistsAccum -= kResistsPeriod;
            _resistsTickCount++;
            if (_resistsTickCount % kResistsClearEvery == 0) a.Resists.Clear();
            // Rotate over a fixed key set so the dict op-log mixes
            // kDictSet (existing key) with kDictSet (new key).
            var key = (_resistsTickCount % 4) switch
            {
                0 => "fire", 1 => "ice", 2 => "lightning", _ => "poison",
            };
            a.Resists[key] = _resistsTickCount * 10;
        }

        _combosAccum += dt;
        while (_combosAccum >= kCombosPeriod)
        {
            _combosAccum -= kCombosPeriod;
            _combosTickCount++;
            if (_combosTickCount % kCombosClearEvery == 0) a.Combos.Clear();
            // list<list<int32>> — inner list must be ObservableList
            // because the outer slot's element type is the wrapper.
            var inner = new global::Atlas.Observable.ObservableList<int>();
            inner.Add(_combosTickCount);
            inner.Add(_combosTickCount + 1);
            inner.Add(_combosTickCount + 2);
            a.Combos.Add(inner);
        }

        _buffsAccum += dt;
        while (_buffsAccum >= kBuffsPeriod)
        {
            _buffsAccum -= kBuffsPeriod;
            _buffsTickCount++;
            if (_buffsTickCount % kBuffsClearEvery == 0) a.Buffs.Clear();
            var b = new StressBuff
            {
                Kind       = _buffsTickCount % 5,
                Stacks     = 1 + (_buffsTickCount % 3),
                DurationMs = (uint)(1000 + _buffsTickCount * 250),
            };
            a.Buffs.Add(b);
        }

        _spellSlotsAccum += dt;
        while (_spellSlotsAccum >= kSpellSlotsPeriod)
        {
            _spellSlotsAccum -= kSpellSlotsPeriod;
            _spellSlotsTickCount++;
            var slot = (_spellSlotsTickCount % 3) switch
            {
                0 => "primary", 1 => "secondary", _ => "ult",
            };
            a.SpellSlots[slot] = new StressWeapon
            {
                Id        = 2000 + _spellSlotsTickCount,
                Sharpness = (ushort)(50 + _spellSlotsTickCount % 50),
                Bound     = (_spellSlotsTickCount & 1) == 0,
            };
        }

        _loadoutsAccum += dt;
        while (_loadoutsAccum >= kLoadoutsPeriod)
        {
            _loadoutsAccum -= kLoadoutsPeriod;
            _loadoutsTickCount++;
            var name = (_loadoutsTickCount % 2 == 0) ? "pve" : "pvp";
            var ol = new global::Atlas.Observable.ObservableList<int>();
            ol.Add(_loadoutsTickCount);
            ol.Add(_loadoutsTickCount * 2);
            ol.Add(_loadoutsTickCount * 3);
            a.Loadouts[name] = ol;
        }

        // Component-side ticks live alongside entity-side ticks so a
        // single timer drives the whole avatar's mutation surface.
        TickLoadComponent(a, dt);

        // Server→client RPCs fire on a separate cadence track. These
        // exercise the container / struct RPC arg path AND component RPC
        // dispatch (OnAffixesUpdated routes through slot_idx in rpc_id).
        // Script clients log receipt so world_stress's ClientEventCounters
        // can attribute coverage.
        TickClientRpcs(a, dt);
    }

    private int _weaponBrokenSeq, _scoresSnapshotSeq, _affixesUpdatedSeq, _areaBroadcastSeq;

    private void TickClientRpcs(StressAvatar a, float dt)
    {
        _onWeaponBrokenAccum += dt;
        while (_onWeaponBrokenAccum >= kOnWeaponBrokenPeriod)
        {
            _onWeaponBrokenAccum -= kOnWeaponBrokenPeriod;
            _weaponBrokenSeq++;
            // Server→client struct-arg RPC. Wire layout uses the struct's
            // own Serialize so this exercises the full struct-arg path.
            a.Client.OnWeaponBroken(new StressWeapon
            {
                Id        = 9000 + _weaponBrokenSeq,
                Sharpness = (ushort)(_weaponBrokenSeq % 100),
                Bound     = (_weaponBrokenSeq & 1) == 0,
            });
        }

        _onScoresSnapshotAccum += dt;
        while (_onScoresSnapshotAccum >= kOnScoresSnapshotPeriod)
        {
            _onScoresSnapshotAccum -= kOnScoresSnapshotPeriod;
            _scoresSnapshotSeq++;
            // Server→client list-arg RPC. Build a fresh List<int> per
            // call — the codegen ships count + values without observable
            // wrappers (RPC arg semantics).
            var snapshot = new System.Collections.Generic.List<int>();
            for (int i = 0; i < 4; i++) snapshot.Add(_scoresSnapshotSeq * 10 + i);
            a.Client.OnScoresSnapshot(snapshot);
        }

        _onAffixesUpdatedAccum += dt;
        while (_onAffixesUpdatedAccum >= kOnAffixesUpdatedPeriod)
        {
            _onAffixesUpdatedAccum -= kOnAffixesUpdatedPeriod;
            _affixesUpdatedSeq++;
            // Server→client COMPONENT RPC — rpc_id encodes slot=1 so the
            // dispatcher routes into StressLoadComponent on the client.
            // The component send-side helper (in ClientReplicatedComponent)
            // computes the rpc_id from _slotIdx + entity TypeId at send.
            var ids = new System.Collections.Generic.List<int>();
            for (int i = 0; i < 3; i++) ids.Add(_affixesUpdatedSeq * 7 + i);
            a.Load!.OnAffixesUpdated(ids);
        }

        _onAreaBroadcastAccum += dt;
        while (_onAreaBroadcastAccum >= kOnAreaBroadcastPeriod)
        {
            _onAreaBroadcastAccum -= kOnAreaBroadcastPeriod;
            _areaBroadcastSeq++;
            // RpcTarget.All — fan-out routes through every Witness queue.
            a.AllClients.OnAreaBroadcast((uint)_areaBroadcastSeq, _areaBroadcastSeq * 3);
        }
    }

    private float _extraHpAccum, _affixesAccum, _resistMapAccum;
    private float _affixGroupsAccum, _offhandAccum, _quickBuffsAccum;
    private int _affixesCount, _resistMapCount, _affixGroupsCount, _quickBuffsCount;

    // Server→client component-RPC drivers. Cadences picked to fire often
    // enough that world_stress's ClientEventCounters land non-zero counts
    // within a 30 s hold window even at 25 clients (Config-S25), but
    // sparse enough that the bytes/s baseline doesn't flip pass/fail.
    private float _onWeaponBrokenAccum, _onScoresSnapshotAccum, _onAffixesUpdatedAccum;
    private float _onAreaBroadcastAccum;
    private const float kOnWeaponBrokenPeriod    = 7.0f;
    private const float kOnScoresSnapshotPeriod  = 9.0f;
    private const float kOnAffixesUpdatedPeriod  = 11.0f;
    private const float kOnAreaBroadcastPeriod   = 8.0f;

    private void TickLoadComponent(StressAvatar a, float dt)
    {
        var load = a.Load;
        if (load is null) return;

        _extraHpAccum += dt;
        while (_extraHpAccum >= 1.0f)
        {
            _extraHpAccum -= 1.0f;
            load.ExtraHp = load.ExtraHp + 1;
        }

        _affixesAccum += dt;
        while (_affixesAccum >= 0.5f)
        {
            _affixesAccum -= 0.5f;
            _affixesCount++;
            if (_affixesCount % 16 == 0) load.Affixes.Clear();
            load.Affixes.Add(_affixesCount);
        }

        _resistMapAccum += dt;
        while (_resistMapAccum >= 3.0f)
        {
            _resistMapAccum -= 3.0f;
            _resistMapCount++;
            var key = (_resistMapCount % 3) switch
            {
                0 => "physical", 1 => "magical", _ => "true",
            };
            load.ResistMap[key] = _resistMapCount;
        }

        _affixGroupsAccum += dt;
        while (_affixGroupsAccum >= 5.0f)
        {
            _affixGroupsAccum -= 5.0f;
            _affixGroupsCount++;
            if (_affixGroupsCount % 5 == 0) load.AffixGroups.Clear();
            var grp = new global::Atlas.Observable.ObservableList<int>();
            grp.Add(_affixGroupsCount);
            grp.Add(_affixGroupsCount + 100);
            load.AffixGroups.Add(grp);
        }

        _offhandAccum += dt;
        while (_offhandAccum >= 1.0f)
        {
            _offhandAccum -= 1.0f;
            // Component struct properties use whole-struct setters today
            // (no MutRef yet), so we read-modify-write the value type.
            var w = load.Offhand;
            w.Id += 1;
            load.Offhand = w;
        }

        _quickBuffsAccum += dt;
        while (_quickBuffsAccum >= 4.0f)
        {
            _quickBuffsAccum -= 4.0f;
            _quickBuffsCount++;
            if (_quickBuffsCount % 6 == 0) load.QuickBuffs.Clear();
            load.QuickBuffs.Add(new StressBuff
            {
                Kind       = 100 + _quickBuffsCount,
                Stacks     = _quickBuffsCount % 4,
                DurationMs = (uint)(500 + _quickBuffsCount * 100),
            });
        }
    }
}
