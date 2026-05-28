using Atlas.Components;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Entity;
using Atlas.Shared.Protocol;

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
    private const ushort kDashSkillId = 1001;
    private const ushort kDashCurveId = 1;
    private const float kDashDistance = 4.0f;
    private const ushort kDashDurationMs = 450;
    private const byte kDashPriority = 10;
    private const uint kSpaceId = 1;
    private static readonly Vector3 kSpawnPosition = new(0f, 0f, 0f);
    private static readonly float[] s_dashCurveSamples = { 0.0f, 1.0f };

    private ProjectileSimulator _sim = null!;

    private bool _isDead;
    private double _deathServerTime;
    private uint _nextMovementCommandId;

    protected override void OnInit(bool isReload)
    {
        // ProjectileSimulator is per-cellapp local state, not in the offload
        // blob; rewire on every arrival (fresh login + offload reload).
        AttachSimulator();
        RegisterMovementCurve(kDashCurveId, s_dashCurveSamples);
        // Component slots are missing from persistent_blob; re-add on arrival or
        // DispatchCellRpc drops EquipWeapon on the post-offload Real.
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

    public partial void Dash(Vector3 forward)
    {
        if (_isDead) return;
        Vector3 start = Position;
        Vector3 direction = HorizontalUnit(forward, Direction);
        Vector3 target = new(start.X + direction.X * kDashDistance,
                             start.Y,
                             start.Z + direction.Z * kDashDistance);
        var command = new MovementCommand(
            NextMovementCommandId(), kDashSkillId, MovementCommandType.Dash,
            start, target, kDashDurationMs, curveId: kDashCurveId,
            inputPolicy: MovementCommandInputPolicy.Suppress,
            collisionPolicy: MovementCommandCollisionPolicy.Stop, priority: kDashPriority);
        _ = SetMovementCommand(command);
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
        _ = ClearMovementCommand();
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

    private uint NextMovementCommandId()
    {
        unchecked
        {
            uint next = ++_nextMovementCommandId;
            if (next == 0) next = ++_nextMovementCommandId;
            return next;
        }
    }

    private static Vector3 HorizontalUnit(Vector3 value, Vector3 fallback)
    {
        var direction = new Vector3(value.X, 0.0f, value.Z);
        if (direction.LengthSquared <= 0.0001f)
            direction = new Vector3(fallback.X, 0.0f, fallback.Z);
        return direction.LengthSquared > 0.0001f ? direction.Normalized : Vector3.Forward;
    }
}
