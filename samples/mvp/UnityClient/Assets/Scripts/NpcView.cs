using UnityEngine;
using MvpNpc = Atlas.Mvp.Client.Npc;

namespace Atlas.Mvp.Unity
{
    public sealed class NpcView : EntityView
    {
        MvpNpc Npc => (MvpNpc)Entity!;

        protected override Color PickAliveColor() => new(0.6f, 0.6f, 0.6f);

        protected override void HookEvents() => Npc.DamageReceived += OnDamage;

        protected override void UnhookEvents()
        {
            if (Entity is MvpNpc n) n.DamageReceived -= OnDamage;
        }

        void OnDamage(int amount, uint attackerId) => SpawnDamageFloater(amount);
    }
}
