using Atlas.Entity;
using Atlas.Serialization;

namespace Atlas.BaseSample;

[Entity("Account")]
public partial class Account : ServerEntity
{
    public override string TypeName => "Account";
    public override void Serialize(ref SpanWriter writer) { }
    public override void Deserialize(ref SpanReader reader) { }

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
