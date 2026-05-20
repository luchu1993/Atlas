using System.Collections.Generic;
using Atlas.Client;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    // Renders BSP leaf rects from the server-pushed SpaceBspGeometry message;
    // each leaf draws as a coloured outline keyed by the owner cellapp.
    public static class BspGizmo
    {
        const float kGroundY = 0.06f;
        const float kLineWidth = 0.25f;
        // Bounds reported by mgr may extend to ±DefaultWorldHalfExtent (1000m);
        // clip to a sane viewable extent so off-world half-finite cells stay
        // inside the gizmo.
        const float kRenderClip = 200f;

        static GameObject? _root;
        static readonly List<GameObject> _leafObjects = new();
        static IReadOnlyList<ClientCallbacks.BspLeafRect>? _latestLeaves;
        public static bool Visible { get; private set; }

        static BspGizmo()
        {
            ClientCallbacks.SpaceBspGeometryReceived += OnGeometry;
        }

        public static void Attach()
        {
            if (_root != null) return;
            _root = new GameObject("BspGizmo");
            _root.SetActive(Visible);
            // Geometry may have arrived pre-Attach; replay the cache so the
            // gizmo paints immediately instead of waiting for the next push.
            if (_latestLeaves != null) RepaintLeaves(_latestLeaves);
        }

        public static void SetVisible(bool visible)
        {
            Visible = visible;
            if (_root != null) _root.SetActive(visible);
        }

        public static void Clear()
        {
            foreach (var go in _leafObjects)
            {
                if (go != null) Object.Destroy(go);
            }
            _leafObjects.Clear();
            if (_root != null) Object.Destroy(_root);
            _root = null;
            _latestLeaves = null;
        }

        static void OnGeometry(uint spaceId, IReadOnlyList<ClientCallbacks.BspLeafRect> leaves)
        {
            _latestLeaves = leaves;
            if (_root != null) RepaintLeaves(leaves);
        }

        static void RepaintLeaves(IReadOnlyList<ClientCallbacks.BspLeafRect> leaves)
        {
            // Pool: reuse existing GameObjects, shrink when leaves count drops.
            while (_leafObjects.Count < leaves.Count) _leafObjects.Add(BuildLeafGo());
            for (int i = leaves.Count; i < _leafObjects.Count; ++i)
            {
                if (_leafObjects[i] != null) _leafObjects[i].SetActive(false);
            }

            for (int i = 0; i < leaves.Count; ++i)
            {
                var go = _leafObjects[i];
                go.SetActive(true);
                var leaf = leaves[i];
                var color = OwnerColor(leaf.OwnerIndex);
                float minX = Mathf.Max(leaf.MinX, -kRenderClip);
                float minZ = Mathf.Max(leaf.MinZ, -kRenderClip);
                float maxX = Mathf.Min(leaf.MaxX, kRenderClip);
                float maxZ = Mathf.Min(leaf.MaxZ, kRenderClip);
                SetRect(go, minX, minZ, maxX, maxZ, color);
            }
        }

        static GameObject BuildLeafGo()
        {
            var go = new GameObject("BspLeaf");
            go.transform.SetParent(_root!.transform, false);
            var lr = go.AddComponent<LineRenderer>();
            lr.useWorldSpace = true;
            lr.positionCount = 5;
            lr.startWidth = kLineWidth;
            lr.endWidth = kLineWidth;
            lr.loop = true;
            lr.material = new Material(Shader.Find("Sprites/Default"));
            return go;
        }

        static void SetRect(GameObject go, float minX, float minZ, float maxX, float maxZ,
                            Color color)
        {
            var lr = go.GetComponent<LineRenderer>();
            lr.SetPosition(0, new Vector3(minX, kGroundY, minZ));
            lr.SetPosition(1, new Vector3(maxX, kGroundY, minZ));
            lr.SetPosition(2, new Vector3(maxX, kGroundY, maxZ));
            lr.SetPosition(3, new Vector3(minX, kGroundY, maxZ));
            lr.SetPosition(4, new Vector3(minX, kGroundY, minZ));
            lr.startColor = color;
            lr.endColor = color;
        }

        // Distinct colors per cellapp owner_index; cycles after 8 hosts.
        static Color OwnerColor(byte ownerIndex)
        {
            return (ownerIndex % 8) switch
            {
                0 => new Color(0.2f, 0.85f, 1f, 0.9f),  // cyan
                1 => new Color(1f, 0.6f, 0.2f, 0.9f),   // orange
                2 => new Color(0.4f, 1f, 0.4f, 0.9f),   // green
                3 => new Color(1f, 0.4f, 0.9f, 0.9f),   // magenta
                4 => new Color(1f, 1f, 0.3f, 0.9f),     // yellow
                5 => new Color(0.6f, 0.4f, 1f, 0.9f),   // purple
                6 => new Color(1f, 0.3f, 0.3f, 0.9f),   // red
                _ => new Color(0.7f, 0.7f, 0.7f, 0.9f), // gray
            };
        }
    }
}
