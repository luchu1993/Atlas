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
        // P3.3: world_stress encodes the desired space_id into avatarIndex.
        // Values <= 0 default to space 1 so the one-space case (all prior
        // P2/P3 smoke runs) keeps working unchanged.
        uint spaceId = avatarIndex > 0 ? (uint)avatarIndex : 1u;
        Log.Info(
            $"[StressTest.Base] Account.SelectAvatar(index={avatarIndex}) entity={EntityId} -> space={spaceId}");

        var avatar = EntityFactory.CreateBase("StressAvatar", spaceId);
        if (avatar == null)
        {
            Log.Error($"[StressTest.Base] SelectAvatar: failed to create StressAvatar");
            return;
        }
        Log.Info(
            $"[StressTest.Base] SelectAvatar: created StressAvatar entity={avatar.EntityId} in space={spaceId}, handing off client");

        // Transfer the client proxy from the Account to the new StressAvatar
        // so cell RPCs (Echo / ReportPos) can reach the avatar directly.
        // BindClient on the BaseApp side triggers the cell to EnableWitness
        // with CellAppConfig defaults (500 m radius, 5 m hysteresis). C5
        // follows up with a SetAoIRadius(50) so stress peers move in and
        // out of AoI against the ±50 m random-walk clamp.
        GiveClientTo(avatar.EntityId);
    }
}
