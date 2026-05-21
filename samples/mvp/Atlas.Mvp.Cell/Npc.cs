using Atlas.Components;
using Atlas.Entity;
using Atlas.Space;

namespace Atlas.Mvp.Cell;

[Entity("Npc")]
public partial class Npc : CellServerEntity, IDamageable
{
    private const int kInitialHp = 100;
    private const uint kSpaceId = 1;

    private ProjectileSimulator _sim = null!;

    internal ProjectileSimulator Simulator => _sim;

    protected override void OnInit(bool isReload)
    {
        // ProjectileSimulator + ServerLocal component are per-cellapp; not
        // carried by Offload, so rewire on every arrival incl. isReload=true.
        AttachSimulator();
        var ai = AddLocalComponent<NpcAiComponent>();
        if (isReload) return;
        // Fresh spawn: seed initial AiTarget / AiFireInterval. Offload-arrived
        // NPCs (isReload=true) inherit these from persistent_blob.
        ai.InitFreshState();
        Hp = kInitialHp;
        NpcType = 1;
        if (SpaceOwnerRegistry.Find(SpaceId) is MvpSpace space) space.NotifyNpcAlive();
    }

    protected override void OnDestroy()
    {
        _sim?.UnregisterTarget(EntityId);
        if (SpaceOwnerRegistry.Find(SpaceId) is MvpSpace space) space.NotifyNpcDead();
    }

    // Ghost mirror on a peer cellapp: register so a projectile fired here can
    // detect a hit; the simulator routes damage to the Real via SendCellRpc.
    protected override void OnGhostInit() => AttachSimulator();
    protected override void OnGhostDestroy() => _sim?.UnregisterTarget(EntityId);

    private void AttachSimulator()
    {
        _sim = ProjectileSimulator.ForSpace(kSpaceId);
        _sim.RegisterTarget(this);
    }

    public partial void TakeDamage(int amount, uint attackerId)
    {
        if (Hp <= 0) return;
        Hp -= amount;
        Atlas.Diagnostics.Log.Info($"[Mvp.Cell] Npc {EntityId} TakeDamage({amount}) by {attackerId} -> Hp={Hp}");
        AllClients.ShowDamage(amount, attackerId);
        if (Hp > 0) return;
        if (EntityManager.Instance.Get(attackerId) is Avatar killer)
            killer.OnNpcKilled();
        DestroySelf();
    }
}
