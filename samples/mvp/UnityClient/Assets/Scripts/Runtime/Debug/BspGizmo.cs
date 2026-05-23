using System;
using System.Collections.Generic;
using Atlas.Client;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public sealed class BspGizmo : IDisposable
    {
        const float kGroundY = 0.05f;
        const float kFillAlpha = 0.28f;
        const float kRenderClip = 200f;

        readonly ClientSession _session;
        readonly List<GameObject> _leafObjects = new();
        GameObject? _root;
        IReadOnlyList<ClientCallbacks.BspLeafRect>? _latestLeaves;
        Mesh? _quadMesh;

        public BspGizmo(ClientSession session)
        {
            _session = session ?? throw new ArgumentNullException(nameof(session));
            _session.SpaceBspGeometryReceived += OnGeometry;
        }

        public bool Visible { get; private set; }

        public void Attach()
        {
            if (_root != null) return;
            _root = new GameObject("BspGizmo");
            _root.SetActive(Visible);
            if (_latestLeaves != null) RepaintLeaves(_latestLeaves);
        }

        public void SetVisible(bool visible)
        {
            Visible = visible;
            if (_root != null) _root.SetActive(visible);
        }

        public void Clear()
        {
            foreach (var go in _leafObjects)
            {
                if (go != null) UnityEngine.Object.Destroy(go);
            }
            _leafObjects.Clear();
            if (_root != null) UnityEngine.Object.Destroy(_root);
            _root = null;
            _latestLeaves = null;
        }

        public void Dispose()
        {
            _session.SpaceBspGeometryReceived -= OnGeometry;
            Clear();
        }

        void OnGeometry(uint spaceId, IReadOnlyList<ClientCallbacks.BspLeafRect> leaves)
        {
            _latestLeaves = leaves;
            if (_root != null) RepaintLeaves(leaves);
        }

        void RepaintLeaves(IReadOnlyList<ClientCallbacks.BspLeafRect> leaves)
        {
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
                float minX = Mathf.Max(leaf.MinX, -kRenderClip);
                float minZ = Mathf.Max(leaf.MinZ, -kRenderClip);
                float maxX = Mathf.Min(leaf.MaxX, kRenderClip);
                float maxZ = Mathf.Min(leaf.MaxZ, kRenderClip);
                SetRect(go, minX, minZ, maxX, maxZ, LeafColor(leaf.OwnerIndex, leaf.Load));
            }
        }

        GameObject BuildLeafGo()
        {
            var go = new GameObject("BspLeaf");
            go.transform.SetParent(_root!.transform, false);
            var mf = go.AddComponent<MeshFilter>();
            mf.sharedMesh = GetOrBuildQuadMesh();
            var mr = go.AddComponent<MeshRenderer>();
            mr.sharedMaterial = new Material(Shader.Find("Sprites/Default"));
            mr.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
            mr.receiveShadows = false;
            return go;
        }

        Mesh GetOrBuildQuadMesh()
        {
            if (_quadMesh != null) return _quadMesh;
            _quadMesh = new Mesh
            {
                vertices = new[]
                {
                    new Vector3(0f, 0f, 0f),
                    new Vector3(1f, 0f, 0f),
                    new Vector3(0f, 0f, 1f),
                    new Vector3(1f, 0f, 1f),
                },
                triangles = new[] { 0, 2, 1, 1, 2, 3 },
                uv = new[] { Vector2.zero, Vector2.right, Vector2.up, Vector2.one },
            };
            _quadMesh.RecalculateNormals();
            _quadMesh.RecalculateBounds();
            return _quadMesh;
        }

        static void SetRect(GameObject go, float minX, float minZ, float maxX, float maxZ,
                            Color color)
        {
            float ex = Mathf.Max(0.01f, maxX - minX);
            float ez = Mathf.Max(0.01f, maxZ - minZ);
            go.transform.localPosition = new Vector3(minX, kGroundY, minZ);
            go.transform.localScale = new Vector3(ex, 1f, ez);
            var mr = go.GetComponent<MeshRenderer>();
            color.a = kFillAlpha;
            mr.material.color = color;
        }

        static Color OwnerColor(byte ownerIndex)
        {
            return (ownerIndex % 8) switch
            {
                0 => new Color(0.2f, 0.85f, 1f, 1f),
                1 => new Color(1f, 0.6f, 0.2f, 1f),
                2 => new Color(0.4f, 1f, 0.4f, 1f),
                3 => new Color(1f, 0.4f, 0.9f, 1f),
                4 => new Color(1f, 1f, 0.3f, 1f),
                5 => new Color(0.6f, 0.4f, 1f, 1f),
                6 => new Color(1f, 0.3f, 0.3f, 1f),
                _ => new Color(0.7f, 0.7f, 0.7f, 1f),
            };
        }

        static Color LeafColor(byte ownerIndex, float load)
        {
            var owner = OwnerColor(ownerIndex);
            var hot = new Color(1f, 0.12f, 0.05f, 1f);
            return Color.Lerp(owner, hot, Mathf.Clamp01(load) * 0.65f);
        }
    }
}
