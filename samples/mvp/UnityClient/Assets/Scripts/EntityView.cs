using System;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using UnityEngine.UI;

namespace Atlas.Mvp.Unity
{
    public abstract class EntityView : ITickable, ILateTickable, IDisposable
    {
        public ClientEntity? Entity { get; private set; }
        public GameObject Root { get; }
        protected AtlasNetworkManager Net { get; }
        protected Transform CapsuleTransform = null!;
        protected Renderer CapsuleRenderer = null!;
        protected Color AliveColor;

        readonly Text _label;
        bool _filterTuned;

        protected EntityView(ClientEntity entity, AtlasNetworkManager net, Transform worldRoot)
        {
            Entity = entity;
            Net = net;
            Root = new GameObject($"View_{entity.TypeName}_{entity.EntityId}");
            Root.transform.SetParent(worldRoot, false);
            SeedTransform();
            BuildVisual();
            _label = LabelOverlay.Instance!.CreateLabel();
            HookEvents();
            Ticker.Add(this);
        }

        public virtual void Dispose()
        {
            Ticker.Remove(this);
            UnhookEvents();
            if (_label != null) UnityEngine.Object.Destroy(_label.gameObject);
            if (Root != null) UnityEngine.Object.Destroy(Root);
        }

        public void Tick(float dt)
        {
            if (Entity == null || Entity.IsDestroyed) return;
            TuneFilterOnce();
            SyncPeerTransform();
            _label.text = ComposeLabelText();
        }

        public void LateTick()
        {
            if (Entity == null || Entity.IsDestroyed || _label == null) return;
            var headWorld = Root.transform.position + Vector3.up * 2.1f;
            var visible = LabelOverlay.Instance!.TryProject(headWorld, out var screen);
            if (visible) _label.rectTransform.anchoredPosition = new Vector2(screen.x, screen.y);
            if (_label.gameObject.activeSelf != visible) _label.gameObject.SetActive(visible);
        }

        // Peers spawn at world origin without this seed; the first interpolated
        // sample arrives a frame later, leaving the capsule at (0,0,0) until then.
        void SeedTransform()
        {
            var p = Entity!.Position;
            Root.transform.position = new Vector3(p.X, p.Y, p.Z);
            var d = Entity.Direction;
            if (d.X * d.X + d.Z * d.Z > 0.0001f)
                Root.transform.rotation = Quaternion.LookRotation(new Vector3(d.X, 0f, d.Z));
        }

        void BuildVisual()
        {
            var capsule = GameObject.CreatePrimitive(PrimitiveType.Capsule);
            capsule.transform.SetParent(Root.transform, false);
            capsule.transform.localPosition = new Vector3(0f, 1f, 0f);
            CapsuleTransform = capsule.transform;
            CapsuleRenderer = capsule.GetComponent<Renderer>();
            AliveColor = PickAliveColor();
            CapsuleRenderer.material.color = AliveColor;

            var nose = GameObject.CreatePrimitive(PrimitiveType.Cube);
            nose.transform.SetParent(Root.transform, false);
            nose.transform.localPosition = new Vector3(0f, 1.0f, 0.55f);
            nose.transform.localScale = new Vector3(0.25f, 0.25f, 0.5f);
            if (nose.TryGetComponent<Collider>(out var col)) UnityEngine.Object.Destroy(col);
            nose.GetComponent<Renderer>().material.color = Color.yellow;
        }

        // MaxExtrapolation must cover the LOD sample gap (≤300 ms for the
        // farthest tier) or the peer freezes and reads as "stuck" on the client.
        void TuneFilterOnce()
        {
            if (_filterTuned || Entity!.Filter is not { } f) return;
            f.ServerInterval = 0.05;
            f.MaxExtrapolation = 0.5;
            _filterTuned = true;
        }

        // Owner transform is written by PlayerInputController; witness skips self.
        void SyncPeerTransform()
        {
            if (Entity!.IsOwner) return;
            if (!Net.TryGetInterpolatedTransform(Entity.EntityId, out var pos, out var dir, out _))
                return;
            Root.transform.position = pos;
            if (dir.sqrMagnitude > 0.01f)
                Root.transform.rotation = Quaternion.LookRotation(dir);
        }

        protected void SpawnDamageFloater(int amount)
        {
            DamageFloater.Spawn(Root.transform.position + Vector3.up * 2.2f, amount);
        }

        protected abstract Color PickAliveColor();
        protected abstract void HookEvents();
        protected abstract void UnhookEvents();
        protected virtual string ComposeLabelText() =>
            $"{Entity!.TypeName}:{Entity.EntityId}";
    }
}
