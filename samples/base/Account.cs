using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.BaseSample;

[Entity("Account")]
public partial class Account : ServerEntity
{
    // Generated from Account.def (base_methods, exposed):
    //   public partial void RequestAvatarList();
    //   public partial void SelectAvatar(int avatarIndex);

    public partial void RequestAvatarList()
    {
        Log.Info($"[Base] Account.RequestAvatarList on entity {EntityId}");
    }

    public partial void SelectAvatar(int avatarIndex)
    {
        Log.Info($"[Base] Account.SelectAvatar(index={avatarIndex}) on entity {EntityId}");
    }
}
