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

        GiveClientTo(avatar.EntityId);
    }
}
