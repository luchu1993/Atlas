using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using UnityEngine.UI;

namespace Atlas.Mvp.Unity
{
    // Run after Bootstrap so label projection sees the post-FollowOwner camera
    // pose; otherwise labels lag one frame behind every camera move.
    [DefaultExecutionOrder(100)]
    public abstract class EntityView : MonoBehaviour
    {
        public ClientEntity? Entity { get; private set; }
        protected AtlasNetworkManager Net = null!;
        protected Transform CapsuleTransform = null!;
        protected Renderer CapsuleRenderer = null!;
        protected Color AliveColor;

        Text _label = null!;
        bool _filterTuned;

        public void Bind(ClientEntity entity, AtlasNetworkManager net)
        {
            Entity = entity;
            Net = net;
            SeedTransform();
            BuildVisual();
            _label = LabelOverlay.Instance!.CreateLabel();
            HookEvents();
        }

        void Update()
        {
            if (Entity == null || Entity.IsDestroyed) return;
            TuneFilterOnce();
            SyncPeerTransform();
            _label.text = ComposeLabelText();
        }

        void LateUpdate()
        {
            if (Entity == null || Entity.IsDestroyed || _label == null) return;
            var headWorld = transform.position + Vector3.up * 2.1f;
            var visible = LabelOverlay.Instance!.TryProject(headWorld, out var screen);
            if (visible) _label.rectTransform.anchoredPosition = new Vector2(screen.x, screen.y);
            if (_label.gameObject.activeSelf != visible) _label.gameObject.SetActive(visible);
        }

        void OnDestroy()
        {
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
            CapsuleTransform = capsule.transform;
            CapsuleRenderer = capsule.GetComponent<Renderer>();
            AliveColor = PickAliveColor();
            CapsuleRenderer.material.color = AliveColor;

            var nose = GameObject.CreatePrimitive(PrimitiveType.Cube);
            nose.transform.SetParent(transform, false);
            nose.transform.localPosition = new Vector3(0f, 1.0f, 0.55f);
            nose.transform.localScale = new Vector3(0.25f, 0.25f, 0.5f);
            if (nose.TryGetComponent<Collider>(out var col)) Object.Destroy(col);
            nose.GetComponent<Renderer>().material.color = Color.yellow;
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
            if (!Net.TryGetInterpolatedTransform(Entity.EntityId, out var pos, out var dir, out _))
                return;
            transform.position = pos;
            if (dir.sqrMagnitude > 0.01f)
                transform.rotation = Quaternion.LookRotation(dir);
        }

        protected void SpawnDamageFloater(int amount)
        {
            DamageFloater.Spawn(transform.position + Vector3.up * 2.2f, amount);
        }

        protected abstract Color PickAliveColor();
        protected abstract void HookEvents();
        protected abstract void UnhookEvents();
        protected virtual string ComposeLabelText() =>
            $"{Entity!.TypeName}:{Entity.EntityId}";
    }
}
