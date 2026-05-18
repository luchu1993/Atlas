using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.Mvp.Cell;

[Entity("Avatar")]
public partial class Avatar : CellServerEntity, IDamageable
{
    private const int kInitialHp = 100;
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
        if (isReload) return;
        Hp = kInitialHp;
        TickInterval = 1;
        _sim = ProjectileSimulator.ForSpace(kSpaceId);
        _sim.RegisterTarget(this);
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
}
