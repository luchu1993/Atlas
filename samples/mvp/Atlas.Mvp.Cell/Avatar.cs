using Atlas.Components;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Entity;
using Atlas.Space;

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

    protected override void OnInit(bool isReload)
    {
        // ProjectileSimulator is per-cellapp local state, not in the offload
        // blob; rewire on every arrival (fresh login + offload reload).
        _sim = ProjectileSimulator.ForSpace(kSpaceId);
        _sim.RegisterTarget(this);
        if (isReload) return;
        Hp = kInitialHp;
        Level = 1;
        TickInterval = 1;
        AddComponent<EquipmentComponent>();
        // SerializeForOwnerClient (full snapshot) does not carry component
        // sections; force a dirty WeaponId so the first delta ships the slot.
        if (Equipment != null) Equipment.WeaponId = 1;

        // Lazy single-instance MvpSpace: fresh-login Avatar on the primary
        // cellapp seeds it; offload-arrival Avatars skip via isReload above.
        if (SpaceOwnerRegistry.Find(kSpaceId) is null) {
            EntityFactory.CreateLocalCell("MvpSpace", kSpaceId, Vector3.Zero,
                                          Vector3.Forward, onGround: false);
        }
    }

    protected override void OnDestroy()
    {
        _sim?.UnregisterTarget(EntityId);
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
        Position = pos;
        Direction = dir;
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

    public void BroadcastDamage(int amount, uint attackerId)
    {
        AllClients.ShowDamage(amount, attackerId);
        if (_isDead || Hp > 0) return;
        Hp = 0;
        _isDead = true;
        _deathServerTime = Atlas.Time.ServerTime;
        AllClients.OnDied(attackerId);
        Log.Info($"[Mvp.Cell] Avatar {EntityId} died (killer={attackerId})");
    }

    private void Respawn()
    {
        _isDead = false;
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
