using System;
using Atlas.Client.Unity;
using UnityEngine;
using MvpNpc = Atlas.Mvp.Client.Npc;

namespace Atlas.Mvp.Unity
{
    public sealed class NpcView : EntityView
    {
        MvpNpc Npc => (MvpNpc)Entity!;
        readonly Func<uint> _ownerIdSource;

        public NpcView(MvpNpc npc, AtlasNetworkManager net, Transform worldRoot,
            AtlasUnityFramePump frame, LabelOverlay labels, Func<uint> ownerIdSource)
            : base(npc, net, worldRoot, frame, labels)
        {
            _ownerIdSource = ownerIdSource;
        }

        protected override Color PickAliveColor() => new(0.6f, 0.6f, 0.6f);

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
