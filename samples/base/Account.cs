using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.BaseSample;

[Entity("Account")]
public partial class Account : BaseServerEntity
{
    public partial void SelectAvatar(int avatarIndex)
    {
        Log.Info($"[Base] Account.SelectAvatar(index={avatarIndex}) on entity {EntityId}");
    }
}
