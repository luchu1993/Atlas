using Atlas.Core;

namespace Atlas.Entity;

// Base-resident entity base — owns the client proxy and base→cell orchestration.
public abstract class BaseServerEntity : ServerEntity
{
    // After-call: subsequent client RPCs target destEntityId, not `this`.
    protected internal void GiveClientTo(uint destEntityId)
    {
        NativeApi.GiveClientTo(EntityId, destEntityId);
    }

    // hysteresis is the leave-band width: enter at radius, leave at
    // radius + hysteresis. Queued until cell EnableWitness lands.
    public void SetAoIRadius(float radius, float hysteresis = 5f)
    {
        NativeApi.SetAoIRadius(EntityId, radius, hysteresis);
    }
}
