namespace Atlas.Entity.Components;

// Server-only component. Doesn't participate in replication and
// doesn't occupy a synced slot — held by the entity in a Type-keyed
// dictionary so scripts can attach AI / loot rollers / anti-cheat
// without polluting the protocol's component slot table.
public abstract class ServerLocalComponent : ComponentBase
{
}
