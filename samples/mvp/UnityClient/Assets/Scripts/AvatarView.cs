using System.Collections.Generic;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using UnityEngine.UI;
using MvpAvatar = Atlas.Mvp.Client.Avatar;
using MvpNpc = Atlas.Mvp.Client.Npc;

namespace Atlas.Mvp.Unity
{
    // Run after Bootstrap so label projection sees the post-FollowOwner camera
    // pose; otherwise labels lag one frame behind every camera move.
    [DefaultExecutionOrder(100)]
    public sealed class AvatarView : MonoBehaviour
    {
        public static readonly List<AvatarView> All = new();

        public ClientEntity? Entity { get; private set; }
        AtlasNetworkManager _net = null!;
        Transform _capsule = null!;
        Renderer _capsuleRenderer = null!;
        Color _aliveColor;
        Text _label = null!;
        bool _filterTuned;

        public void Bind(ClientEntity entity, AtlasNetworkManager net)
        {
            Entity = entity;
            _net = net;
            SeedTransform();
            BuildVisual();
            _label = LabelOverlay.Instance!.CreateLabel();
            All.Add(this);
            HookEvents();
        }

        void Update()
        {
            if (Entity == null || Entity.IsDestroyed) return;
            TuneFilterOnce();
            SyncPeerTransform();
            UpdateLabelText();
        }

        void LateUpdate()
        {
            if (Entity == null || Entity.IsDestroyed) return;
            if (_label == null) return;
            var headWorld = transform.position + Vector3.up * 2.1f;
            var visible = LabelOverlay.Instance!.TryProject(headWorld, out var screen);
            if (visible) _label.rectTransform.anchoredPosition = new Vector2(screen.x, screen.y);
            if (_label.gameObject.activeSelf != visible) _label.gameObject.SetActive(visible);
        }

        void OnDestroy()
        {
            All.Remove(this);
            if (_label != null) Destroy(_label.gameObject);
            UnhookEvents();
        }

        // Peers spawn at world origin without this seed; the first interpolated
        // sample arrives a frame later, leaving the capsule at (0,0,0) until then.
        void SeedTransform()
        {
            var p = Entity!.Position;
            transform.position = new Vector3(p.X, p.Y, p.Z);
            var d = Entity.Direction;
            if (d.X * d.X + d.Z * d.Z > 0.0001f)
                transform.rotation = Quaternion.LookRotation(new Vector3(d.X, 0f, d.Z));
        }

        void BuildVisual()
        {
            var capsule = GameObject.CreatePrimitive(PrimitiveType.Capsule);
            capsule.transform.SetParent(transform, false);
            capsule.transform.localPosition = new Vector3(0f, 1f, 0f);
            _capsule = capsule.transform;
            _capsuleRenderer = capsule.GetComponent<Renderer>();
            _aliveColor = Entity!.IsOwner ? Color.cyan
                        : Entity is MvpNpc ? new Color(0.6f, 0.6f, 0.6f)
                                            : new Color(0.9f, 0.3f, 0.3f);
            _capsuleRenderer.material.color = _aliveColor;

            var nose = GameObject.CreatePrimitive(PrimitiveType.Cube);
            nose.transform.SetParent(transform, false);
            nose.transform.localPosition = new Vector3(0f, 1.0f, 0.55f);
            nose.transform.localScale = new Vector3(0.25f, 0.25f, 0.5f);
            if (nose.TryGetComponent<Collider>(out var col)) Object.Destroy(col);
            nose.GetComponent<Renderer>().material.color = Color.yellow;
        }

        void HookEvents()
        {
            switch (Entity)
            {
                case MvpAvatar a:
                    a.DamageReceived += OnDamage;
                    a.Died += _ => ApplyDeadVisual(true);
                    a.Respawned += _ => ApplyDeadVisual(false);
                    break;
                case MvpNpc n:
                    n.DamageReceived += OnDamage;
                    break;
            }
        }

        void UnhookEvents()
        {
            switch (Entity)
            {
                case MvpAvatar a: a.DamageReceived -= OnDamage; break;
                case MvpNpc n: n.DamageReceived -= OnDamage; break;
            }
        }

        // Tuned for close-LOD (50 ms cellapp tick); far entities are too small
        // to expose the stutter from the short extrapolation window.
        void TuneFilterOnce()
        {
            if (_filterTuned || Entity!.Filter is not { } f) return;
            f.ServerInterval = 0.05;
            f.MaxExtrapolation = 0.02;
            _filterTuned = true;
        }

        // Owner transform is written by PlayerInputController; witness skips self.
        void SyncPeerTransform()
        {
            if (Entity!.IsOwner) return;
            if (!_net.TryGetInterpolatedTransform(Entity.EntityId, out var pos, out var dir, out _))
                return;
            transform.position = pos;
            if (dir.sqrMagnitude > 0.01f)
                transform.rotation = Quaternion.LookRotation(dir);
        }

        void UpdateLabelText()
        {
            int hp = Entity switch { MvpAvatar a => a.Hp, MvpNpc n => n.Hp, _ => 0 };
            string hpText = Entity is MvpAvatar { IsDead: true } ? "DEAD" : $"HP {hp}";
            _label.text = $"{Entity!.TypeName}:{Entity.EntityId}  {hpText}";
        }

        void ApplyDeadVisual(bool dead)
        {
            if (_capsuleRenderer == null) return;
            _capsuleRenderer.material.color = dead ? new Color(0.3f, 0.3f, 0.3f, 0.6f) : _aliveColor;
            _capsule.localRotation = dead ? Quaternion.Euler(90f, 0f, 0f) : Quaternion.identity;
        }

        void OnDamage(int amount, uint attackerId)
        {
            DamageFloater.Spawn(transform.position + Vector3.up * 2.2f, amount);
        }
    }
}
