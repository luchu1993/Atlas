using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.StressTest.Base;

[Entity("Account")]
public partial class Account : BaseServerEntity
{
    public partial void SelectAvatar(int avatarIndex)
    {
        const int SpaceMask = 0xFFFF;
        int spawnCode = avatarIndex > 0 ? avatarIndex >> 16 : 0;
        uint spaceId = avatarIndex > 0 ? (uint)(avatarIndex & SpaceMask) : 1u;
        if (spaceId == 0) spaceId = 1u;
        Log.Info(
            $"[StressTest.Base] Account.SelectAvatar(index={avatarIndex}) entity={EntityId} -> space={spaceId}");

        var avatar = spawnCode > 0
            ? EntityFactory.CreateBase("StressAvatar", spaceId, SpawnPosition(spawnCode),
                Vector3.Forward, true)
            : EntityFactory.CreateBase("StressAvatar", spaceId);
        if (avatar == null)
        {
            Log.Error($"[StressTest.Base] SelectAvatar: failed to create StressAvatar");
            return;
        }
        Log.Info(
            $"[StressTest.Base] SelectAvatar: created StressAvatar entity={avatar.EntityId} in space={spaceId}, handing off client");

        GiveClientTo(avatar.EntityId);
        avatar.SetAoIRadius(50f, 5f);
    }

    static Vector3 SpawnPosition(int code)
    {
        const float Offset = 300f;
        int quadrant = code > 1 ? code - 1 : 0;
        float x = (quadrant & 1) == 0 ? -Offset : Offset;
        float z = (quadrant & 2) == 0 ? -Offset : Offset;
        return new Vector3(x, 0f, z);
    }
}
