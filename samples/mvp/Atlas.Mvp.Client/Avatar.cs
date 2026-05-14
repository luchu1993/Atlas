using System;
using Atlas.Client;
using Atlas.DataTypes;

namespace Atlas.Mvp.Client;

[Atlas.Entity.Entity("Avatar")]
public partial class Avatar : ClientEntity
{
    public event Action<int, uint>? DamageReceived;
    public event Action<uint>? Died;
    public event Action<Vector3>? Respawned;
    public bool IsDead { get; private set; }

    public partial void ShowDamage(int amount, uint attackerId) =>
        DamageReceived?.Invoke(amount, attackerId);

    public partial void OnDied(uint attackerId)
    {
        IsDead = true;
        Died?.Invoke(attackerId);
    }

    public partial void OnRespawned(Vector3 pos)
    {
        IsDead = false;
        // Drop the lerp ring so the upcoming volatile snap to spawn isn't
        // interpolated from the death position.
        ResetInterpolation();
        Respawned?.Invoke(pos);
    }

    public partial void OnProjectileFired(uint shotId, Vector3 origin, Vector3 velocity) =>
        ProjectileBus.RaiseFired(shotId, EntityId, origin, velocity);

    public partial void OnProjectileEnded(uint shotId, Vector3 endPos, uint hitTargetId) =>
        ProjectileBus.RaiseEnded(shotId, endPos, hitTargetId);
}
