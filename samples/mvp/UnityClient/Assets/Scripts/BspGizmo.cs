using UnityEngine;

namespace Atlas.Mvp.Unity
{
    // Cross at x=0 / z=0 on the ground; matches BootstrapMultiCellPartition's
    // hard-coded split positions and MvpSpace.kWorldHalf (100m).
    public static class BspGizmo
    {
        const float kHalfExtent = 100f;
        const float kLineWidth = 0.25f;
        const float kGroundY = 0.06f;

        static GameObject? _root;
        public static bool Visible { get; private set; }

        public static void Attach()
        {
            if (_root != null) return;
            _root = new GameObject("BspGizmo");
            BuildLine("AxisX", new Vector3(-kHalfExtent, kGroundY, 0f),
                                new Vector3( kHalfExtent, kGroundY, 0f),
                                new Color(0.2f, 0.85f, 1f, 0.9f));
            BuildLine("AxisZ", new Vector3(0f, kGroundY, -kHalfExtent),
                                new Vector3(0f, kGroundY,  kHalfExtent),
                                new Color(1f, 0.6f, 0.2f, 0.9f));
            _root.SetActive(Visible);
        }

        public static void SetVisible(bool visible)
        {
            Visible = visible;
            if (_root != null) _root.SetActive(visible);
        }

        public static void Clear()
        {
            if (_root != null) Object.Destroy(_root);
            _root = null;
        }

        static void BuildLine(string label, Vector3 a, Vector3 b, Color color)
        {
            var go = new GameObject(label);
            go.transform.SetParent(_root!.transform, false);
            var lr = go.AddComponent<LineRenderer>();
            lr.useWorldSpace = true;
            lr.positionCount = 2;
            lr.startWidth = kLineWidth;
            lr.endWidth = kLineWidth;
            lr.material = new Material(Shader.Find("Sprites/Default"));
            lr.startColor = color;
            lr.endColor = color;
            lr.SetPosition(0, a);
            lr.SetPosition(1, b);
        }
    }
}
