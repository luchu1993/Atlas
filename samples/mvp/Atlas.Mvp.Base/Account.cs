using System;
using Atlas.Diagnostics;
using Atlas.Entity;

namespace Atlas.Mvp.Base;

[Entity("Account")]
public partial class Account : BaseServerEntity
{
    public partial void SelectAvatar(int avatarIndex)
    {
        const uint kSpaceId = 1;

        // Captured BEFORE += so the label shows the previous session time stamp,
        // and computed in SelectAvatar (not OnInit) because Atlas reuses the
        // Account entity on fast relogin and skips OnInit on that path.
        string previousLastLogin = LastLogin;
        LoginCount += 1;
        LastLogin = DateTime.UtcNow.ToString("o");
        Log.Info($"[Mvp.Base] Account {EntityId} login #{LoginCount} at {LastLogin}");
        // Detached-after-GiveClientTo Accounts aren't covered by BaseApp's
        // logoff-snapshot persist path; flush state to DBApp ourselves.
        PersistToDb();

        var avatar = EntityFactory.CreateBase("Avatar", kSpaceId);
        if (avatar == null)
        {
            Log.Error("[Mvp.Base] SelectAvatar: failed to create Avatar");
            return;
        }

        GiveClientTo(avatar.EntityId);

        // PublishReplicationFrame is no-op on BaseApp (no base→client delta path);
        // push session info via the owner-targeted ClientRpc instead.
        if (avatar is Avatar mvpAvatar)
        {
            string when = string.IsNullOrEmpty(previousLastLogin) || previousLastLogin.Length < 16
                              ? "first"
                              : previousLastLogin.Substring(5, 11);
            mvpAvatar.OnSessionInfo($"#{LoginCount} · {when}");
        }
    }
}
