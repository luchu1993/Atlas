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
        if (isReload) return;
        // Position is supplied at spawn so the AoI Enter envelope already
        // carries the scattered coord.
        Hp = kInitialHp;
        NpcType = 1;
        _sim = ProjectileSimulator.ForSpace(kSpaceId);
        _sim.RegisterTarget(this);
        AddLocalComponent<NpcAiComponent>();
        if (SpaceOwnerRegistry.Find(SpaceId) is MvpSpace space) space.NotifyNpcAlive();
    }

    protected override void OnDestroy()
    {
        _sim?.UnregisterTarget(EntityId);
        if (SpaceOwnerRegistry.Find(SpaceId) is MvpSpace space) space.NotifyNpcDead();
    }

    public void BroadcastDamage(int amount, uint attackerId)
    {
        AllClients.ShowDamage(amount, attackerId);
        if (Hp <= 0) DestroySelf();
    }
}
