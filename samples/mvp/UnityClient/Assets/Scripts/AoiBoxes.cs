using System.Collections.Generic;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public static class AoiBoxes
    {
        const float kLineWidth = 0.15f;

        static readonly List<GameObject> _roots = new();
        public static bool Visible { get; private set; }

        // Boxes parent under the owner view's transform, so they follow the
        // capsule without needing a per-frame position copy.
        public static void Attach(Transform parent, float inner, float outer,
                                  Color innerColor, Color outerColor)
        {
            var root = new GameObject("AoIBoxes");
            root.transform.SetParent(parent, false);
            BuildBox(root.transform, "Inner", inner, innerColor);
            BuildBox(root.transform, "Outer", outer, outerColor);
            root.SetActive(Visible);
            _roots.Add(root);
        }

        public static void SetVisible(bool visible)
        {
            Visible = visible;
            for (int i = _roots.Count - 1; i >= 0; --i)
            {
                var go = _roots[i];
                if (go == null) { _roots.RemoveAt(i); continue; }
                go.SetActive(visible);
            }
        }

        public static void Clear()
        {
            foreach (var go in _roots)
                if (go != null) Object.Destroy(go);
            _roots.Clear();
        }

        static void BuildBox(Transform parent, string label, float halfExtent, Color color)
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
