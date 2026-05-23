using Atlas.Components;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.Mvp.Cell;

[Entity("Avatar")]
public partial class Avatar : CellServerEntity, IDamageable
{
    private const int kInitialHp = 100;
    private const int kGoldPerKill = 10;
    private const int kGoldPerLevel = 50;
    private const float kRespawnSeconds = 3.0f;
    private const float kProjectileHorizSpeed = 12f;
    private const float kProjectileUpSpeed = 4f;
    private const uint kSpaceId = 1;
    private static readonly Vector3 kSpawnPosition = new(0f, 0f, 0f);

    private ProjectileSimulator _sim = null!;

    private bool _isDead;
    private double _deathServerTime;
    private bool _lastReportMoved;

    protected override void OnInit(bool isReload)
    {
        // ProjectileSimulator is per-cellapp local state, not in the offload
        // blob; rewire on every arrival (fresh login + offload reload).
        AttachSimulator();
        // Component slots are also missing from persistent_blob: re-add on
        // every arrival or DispatchCellRpc silently drops EquipWeapon when
        // _replicated[slot] is null on the post-offload Real.
        var eq = AddComponent<EquipmentComponent>();
        if (isReload) return;
        Hp = kInitialHp;
        Level = 1;
        TickInterval = 1;
        // SerializeForOwnerClient (full snapshot) does not carry component
        // sections; force a dirty WeaponId so the first delta ships the slot.
        eq.WeaponId = 1;
    }

    protected override void OnDestroy()
    {
        _sim?.UnregisterTarget(EntityId);
    }

    // Ghost mirror on a peer cellapp: register so an NPC firing on this cell
    // can hit the Avatar; the simulator routes damage to the Real via SendCellRpc.
    protected override void OnGhostInit() => AttachSimulator();
    protected override void OnGhostDestroy() => _sim?.UnregisterTarget(EntityId);

    private void AttachSimulator()
    {
        _sim = ProjectileSimulator.ForSpace(kSpaceId);
        _sim.RegisterTarget(this);
    }

    protected override void OnTick(float dt)
    {
        _sim.Tick();
        if (!_isDead) return;
        if (Atlas.Time.ServerTime - _deathServerTime < kRespawnSeconds) return;
        Respawn();
    }

    public partial void ReportPos(Vector3 pos, Vector3 dir)
    {
        if (_isDead) return;
        bool moved = (pos - Position).LengthSquared > 0.0001f ||
                     (dir - Direction).LengthSquared > 0.0001f;
        Position = pos;
        Direction = dir;
        if (!moved && _lastReportMoved) MarkVolatileDirty();
        _lastReportMoved = moved;
    }

    public partial void LaunchProjectile(Vector3 forward)
    {
        if (_isDead) return;
        var origin = new Vector3(Position.X + forward.X,
                                 Position.Y + 1.2f,
                                 Position.Z + forward.Z);
        var velocity = new Vector3(forward.X * kProjectileHorizSpeed,
                                   kProjectileUpSpeed,
                                   forward.Z * kProjectileHorizSpeed);
        var shotId = _sim.Register(this, origin, velocity);
        AllClients.OnProjectileFired(shotId, origin, velocity);
    }

    public partial void Say(string text)
    {
        if (_isDead || string.IsNullOrWhiteSpace(text)) return;
        const int kMaxChars = 200;
        if (text.Length > kMaxChars) text = text.Substring(0, kMaxChars);
        AllClients.OnChat(EntityId, text);
    }

    public partial void TakeDamage(int amount, uint attackerId)
    {
        if (_isDead || Hp <= 0) return;
        Hp -= amount;
        AllClients.ShowDamage(amount, attackerId);
        if (Hp > 0) return;
        Hp = 0;
        _isDead = true;
        _deathServerTime = Atlas.Time.ServerTime;
        AllClients.OnDied(attackerId);
        Log.Info($"[Mvp.Cell] Avatar {EntityId} died (killer={attackerId})");
    }

    private void Respawn()
    {
        _isDead = false;
        _lastReportMoved = false;
        Hp = kInitialHp;
        Position = kSpawnPosition;
        AllClients.OnRespawned(kSpawnPosition);
    }

    public void OnNpcKilled()
    {
        Gold += kGoldPerKill;
        int targetLevel = 1 + Gold / kGoldPerLevel;
        if (targetLevel > Level) Level = targetLevel;
    }
}
