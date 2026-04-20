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
        // P4: always ask the cell to enable an AoI witness on the new
        // StressAvatar. 50 m roughly matches the ReportPos random-walk
        // clamp (±50 m square) so neighbours move in and out of AoI
        // during a run; no good way yet to thread this radius from the
        // world_stress CLI — future refinement.
        const float kAoIRadius = 50f;
        Log.Info(
            $"[StressTest.Base] Account.SelectAvatar(index={avatarIndex}) entity={EntityId} -> space={spaceId} aoi={kAoIRadius}");

        var avatar = EntityFactory.CreateBase("StressAvatar", spaceId, kAoIRadius);
        if (avatar == null)
        {
            Log.Error($"[StressTest.Base] SelectAvatar: failed to create StressAvatar");
            return;
        }
        Log.Info(
            $"[StressTest.Base] SelectAvatar: created StressAvatar entity={avatar.EntityId} in space={spaceId}, handing off client");

        // Transfer the client proxy from the Account to the new StressAvatar
        // so cell RPCs (Echo / ReportPos) can reach the avatar directly.
        GiveClientTo(avatar.EntityId);
    }
}
