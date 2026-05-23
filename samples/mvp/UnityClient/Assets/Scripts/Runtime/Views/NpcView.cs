using System;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using MvpNpc = Atlas.Mvp.Client.Npc;

namespace Atlas.Mvp.Unity
{
    public sealed class NpcView : EntityView
    {
        const float kPositionSmoothTime = 0.06f;
        const float kSnapDistanceSq = 4f;
        const float kRotationSharpness = 14f;

        MvpNpc Npc => (MvpNpc)Entity!;
        readonly Func<uint> _ownerIdSource;
        Vector3 _smoothVelocity;
        bool _hasSmoothState;

        public NpcView(MvpNpc npc, AtlasNetworkManager net, Transform worldRoot,
            AtlasUnityFramePump frame, LabelOverlay labels, Func<uint> ownerIdSource)
            : base(npc, net, worldRoot, frame, labels)
        {
            _ownerIdSource = ownerIdSource;
        }

        protected override Color PickAliveColor() => new(0.6f, 0.6f, 0.6f);

        protected override void ConfigureFilter(AvatarFilter f)
        {
            f.ServerInterval = 0.05;
            f.LatencyFrames = 5.0;
            f.MaxExtrapolation = 0.10;
        }

        protected override void ApplyReplicatedTransform(Vector3 pos, Vector3 dir, float dt)
        {
            var current = Root.transform.position;
            if (!_hasSmoothState || (current - pos).sqrMagnitude > kSnapDistanceSq || dt <= 0f)
            {
                _smoothVelocity = Vector3.zero;
                Root.transform.position = pos;
                _hasSmoothState = true;
                if (dir.sqrMagnitude > 0.01f)
                    Root.transform.rotation = Quaternion.LookRotation(dir);
                return;
            }

            Root.transform.position = Vector3.SmoothDamp(
                current, pos, ref _smoothVelocity, kPositionSmoothTime, Mathf.Infinity, dt);

            if (dir.sqrMagnitude > 0.01f)
            {
                var target = Quaternion.LookRotation(dir);
                Root.transform.rotation = Quaternion.Slerp(
                    Root.transform.rotation, target, 1f - Mathf.Exp(-kRotationSharpness * dt));
            }
        }

        protected override void HookEvents() => Npc.DamageReceived += OnDamage;

        protected override void UnhookEvents()
        {
            if (Entity is MvpNpc n) n.DamageReceived -= OnDamage;
        }

        void OnDamage(int amount, uint attackerId)
        {
            if (attackerId == _ownerIdSource())
                SpawnDamageFloater(amount);
        }
    }
}
