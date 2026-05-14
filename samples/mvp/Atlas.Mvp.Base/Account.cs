using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.Mvp.Base;

[Entity("Account")]
public partial class Account : BaseServerEntity
{
    public partial void SelectAvatar(int avatarIndex)
    {
        const uint kSpaceId = 1;

        var avatar = EntityFactory.CreateBase("Avatar", kSpaceId);
        if (avatar == null)
        {
            Log.Error("[Mvp.Base] SelectAvatar: failed to create Avatar");
            return;
        }

        // Bind client before NPCs spawn so witness is up when peers stream in.
        GiveClientTo(avatar.EntityId);
        WorldBootstrap.EnsureSpawned(kSpaceId);
    }
}
