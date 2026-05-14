using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public sealed class AoIDebugRing : MonoBehaviour
    {
        const float kLineWidth = 0.15f;
        public static bool Visible { get; private set; }
        Transform _boxRoot = null!;

        public static void SetVisible(bool visible)
        {
            Visible = visible;
            foreach (var ring in FindObjectsByType<AoIDebugRing>(FindObjectsSortMode.None))
                ring.ApplyVisibility();
        }

        public void Configure(float innerRadius, float outerRadius, Color innerColor, Color outerColor)
        {
            var root = new GameObject($"AoIBoxes.{name}");
            _boxRoot = root.transform;
            BuildBox(_boxRoot, "Inner", innerRadius, innerColor);
            BuildBox(_boxRoot, "Outer", outerRadius, outerColor);
            ApplyVisibility();
        }

        void Update()
        {
            if (_boxRoot != null) _boxRoot.position = transform.position;
        }

        void ApplyVisibility()
        {
            if (_boxRoot != null) _boxRoot.gameObject.SetActive(Visible);
        }

        void OnDestroy()
        {
            if (_boxRoot != null) Destroy(_boxRoot.gameObject);
        }

        void BuildBox(Transform parent, string label, float halfExtent, Color color)
        {
            var go = new GameObject(label);
            go.transform.SetParent(parent, false);
            go.transform.localPosition = new Vector3(0f, 0.05f, 0f);
            var lr = go.AddComponent<LineRenderer>();
            lr.useWorldSpace = false;
            lr.loop = true;
            lr.positionCount = 4;
            lr.startWidth = kLineWidth;
            lr.endWidth = kLineWidth;
            lr.material = new Material(Shader.Find("Sprites/Default"));
            lr.startColor = color;
            lr.endColor = color;
            lr.SetPosition(0, new Vector3(-halfExtent, 0f, -halfExtent));
            lr.SetPosition(1, new Vector3( halfExtent, 0f, -halfExtent));
            lr.SetPosition(2, new Vector3( halfExtent, 0f,  halfExtent));
            lr.SetPosition(3, new Vector3(-halfExtent, 0f,  halfExtent));
        }
    }
}
