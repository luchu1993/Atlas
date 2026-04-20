using Atlas.Entity;

namespace Atlas.StressTest.Base;

// Login entity for the world_stress harness. Account itself is base-only
// (see entity_defs/Account.def — no cell_methods, no cell-scoped props).
// After authenticate, the client calls SelectAvatar to spawn a
// StressAvatar (has_cell=true), and the client proxy is handed over to
// the new avatar so subsequent cell RPCs target it.
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

        var avatar = EntityFactory.CreateBase("StressAvatar");
        if (avatar == null)
        {
            Log.Error($"[StressTest.Base] SelectAvatar: failed to create StressAvatar");
            return;
        }
        Log.Info(
            $"[StressTest.Base] SelectAvatar: created StressAvatar entity={avatar.EntityId}, handing off client");

        // Transfer the client proxy from the Account to the new StressAvatar
        // so cell RPCs (Echo / ReportPos) can reach the avatar directly.
        GiveClientTo(avatar.EntityId);
    }
}
