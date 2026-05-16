using Atlas.Client.Unity;
using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

namespace Atlas.Mvp.Unity
{
    public sealed class AvatarView : EntityView
    {
        MvpAvatar Avatar => (MvpAvatar)Entity!;
        PlayerInputController? _input;

        public AvatarView(MvpAvatar avatar, AtlasNetworkManager net, Transform worldRoot)
            : base(avatar, net, worldRoot) { }

        public void AttachInput(PlayerInputController input) => _input = input;

        public override void Dispose()
        {
            _input?.Dispose();
            _input = null;
            base.Dispose();
        }

        protected override Color PickAliveColor() =>
            Entity!.IsOwner ? Color.cyan : new Color(0.9f, 0.3f, 0.3f);

        protected override void HookEvents()
        {
            var a = Avatar;
            a.DamageReceived += OnDamage;
            a.Died += _ => ApplyDeadVisual(true);
            a.Respawned += _ => ApplyDeadVisual(false);
        }

        protected override void UnhookEvents()
        {
            if (Entity is MvpAvatar a) a.DamageReceived -= OnDamage;
        }

        protected override string ComposeLabelText()
        {
            string suffix = Avatar.IsDead ? "  DEAD" : "";
            return $"{Entity!.TypeName}:{Entity.EntityId}{suffix}";
        }

        void OnDamage(int amount, uint attackerId) => SpawnDamageFloater(amount);

        void ApplyDeadVisual(bool dead)
        {
            if (CapsuleRenderer == null) return;
            CapsuleRenderer.material.color =
                dead ? new Color(0.3f, 0.3f, 0.3f, 0.6f) : AliveColor;
            CapsuleTransform.localRotation =
                dead ? Quaternion.Euler(90f, 0f, 0f) : Quaternion.identity;
        }
    }
}
