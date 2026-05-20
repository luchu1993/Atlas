using System;
using Atlas.Core;
using Atlas.Serialization;

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

    // Snapshot full entity state to DBApp. Use for entities outside BaseApp's
    // automatic logoff-snapshot path (e.g. an Account that handed off the
    // client via GiveClientTo); a no-op on non-BaseApp process types.
    public void PersistToDb()
    {
        var w = new SpanWriter(256);
        try
        {
            Serialize(ref w);
            NativeApi.WriteEntityToDb(EntityId, w.WrittenSpan);
        }
        finally { w.Dispose(); }
    }
}
