using System;
using Atlas.Client;
using Atlas.DataTypes;

namespace Atlas.Mvp.Client;

[Atlas.Entity.Entity("Npc")]
public partial class Npc : ClientEntity
{
    public event Action<int, uint>? DamageReceived;

    public partial void ShowDamage(int amount, uint attackerId) =>
        DamageReceived?.Invoke(amount, attackerId);

    public partial void OnProjectileFired(uint shotId, Vector3 origin, Vector3 velocity) =>
        ProjectileBus.RaiseFired(shotId, EntityId, origin, velocity);

    public partial void OnProjectileEnded(uint shotId, Vector3 endPos, uint hitTargetId) =>
        ProjectileBus.RaiseEnded(shotId, endPos, hitTargetId);
}
