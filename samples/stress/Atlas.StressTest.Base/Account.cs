using Atlas.Entity;

namespace Atlas.StressTest.Base;

// Login entity for the world_stress harness. Account itself is base-only
// (see entity_defs/Account.def — no cell_methods, no cell-scoped props).
// After authenticate, the client is expected to call SelectAvatar to
// spawn a StressAvatar, which is the actual cell-bearing world entity.
//
// P2.2: SelectAvatar is a no-op stub. P2.3 will create the StressAvatar
// entity and transfer the client proxy to it.
[Entity("Account")]
public partial class Account : ServerEntity
{
    public partial void RequestAvatarList()
    {
        Log.Info($"[StressTest.Base] Account.RequestAvatarList entity={EntityId}");
    }

    public partial void SelectAvatar(int avatarIndex)
    {
        Log.Info($"[StressTest.Base] Account.SelectAvatar(index={avatarIndex}) entity={EntityId}");
        // P2.3: EntityFactory.Create("StressAvatar") + client proxy handoff.
    }
}
