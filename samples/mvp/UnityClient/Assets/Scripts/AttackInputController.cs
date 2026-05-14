using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

namespace Atlas.Mvp.Unity
{
    public sealed class AttackInputController : MonoBehaviour
    {
        const float kRange = 30f;
        const int kSkillId = 1;

        MvpAvatar _avatar = null!;

        public void Bind(MvpAvatar avatar) => _avatar = avatar;

        void Update()
        {
            if (_avatar == null || _avatar.IsDestroyed) return;
            if (_avatar.IsDead) return;
            if (!Input.GetMouseButtonDown(0)) return;

            var target = FindNearestTarget();
            if (target == null)
            {
                Debug.Log("[Mvp.Attack] no target in range");
                return;
            }
            _avatar.Cell.CastSkill(kSkillId, target.Entity!.EntityId);
            Debug.Log($"[Mvp.Attack] CastSkill -> {target.Entity.TypeName}:{target.Entity.EntityId}");
        }

        AvatarView? FindNearestTarget()
        {
            AvatarView? best = null;
            float bestDist = kRange;
            var origin = transform.position;
            foreach (var view in AvatarView.All)
            {
                if (view == null || view.Entity == null) continue;
                if (view.Entity.IsOwner) continue;
                float d = Vector3.Distance(origin, view.transform.position);
                if (d < bestDist) { bestDist = d; best = view; }
            }
            return best;
        }
    }
}
