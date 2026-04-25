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
        // world_stress encodes the desired space_id into avatarIndex so
        // the harness can spread clients across multiple spaces with one
        // login flow. Values <= 0 fall back to space 1 — keeps single-space
        // smoke runs working without harness changes.
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
        // BindClient on the BaseApp side fires cellapp::EnableWitness so
        // the avatar's witness comes up with CellAppConfig defaults
        // (500 m radius, 5 m hysteresis).
        GiveClientTo(avatar.EntityId);

        // Shrink the AoI to 50 m so the avatar's ±50 m random-walk
        // clamp in ReportPos produces peers moving in and out of AoI
        // over the course of a run — the property sync / enter / leave
        // stress assertions all depend on that churn. 5 m hysteresis
        // keeps the dual-band trigger from flapping at the boundary.
        //
        // This runs strictly AFTER GiveClientTo so BindClient has
        // already dispatched EnableWitness on the cell; both messages
        // share the same BaseApp→CellApp channel so EnableWitness is
        // guaranteed to land before SetAoIRadius.
        avatar.SetAoIRadius(50f, 5f);
    }
}
