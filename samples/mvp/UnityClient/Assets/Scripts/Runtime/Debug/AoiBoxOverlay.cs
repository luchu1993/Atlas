using System;
using System.Collections.Generic;
using Atlas.Client.Unity;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public sealed class AoiBoxOverlay : IDisposable
    {
        const float kLineWidth = 0.15f;

        readonly struct Entry
        {
            public readonly Transform Target;
            public readonly GameObject Root;
            public Entry(Transform target, GameObject root) { Target = target; Root = root; }
        }

        sealed class Follower : IAtlasUnityTickable
        {
            readonly AoiBoxOverlay _owner;

            public Follower(AoiBoxOverlay owner) => _owner = owner;

            public void Tick(float dt) => _owner.FollowTargets();
        }

        readonly AtlasUnityFramePump _frame;
        readonly List<Entry> _entries = new();
        readonly Follower _follower;
        bool _attached;

        public AoiBoxOverlay(AtlasUnityFramePump frame)
        {
            _frame = frame ?? throw new ArgumentNullException(nameof(frame));
            _follower = new Follower(this);
        }

        public bool Visible { get; private set; }

        public void Attach(Transform target, float inner, float outer,
                           Color innerColor, Color outerColor)
        {
            EnsureFollower();
            var root = new GameObject("AoIBoxes");
            root.transform.position = target.position;
            BuildBox(root.transform, "Inner", inner, innerColor);
            BuildBox(root.transform, "Outer", outer, outerColor);
            root.SetActive(Visible);
            _entries.Add(new Entry(target, root));
        }

        public void SetVisible(bool visible)
        {
            Visible = visible;
            for (int i = _entries.Count - 1; i >= 0; --i)
            {
                var root = _entries[i].Root;
                if (root == null) { _entries.RemoveAt(i); continue; }
                root.SetActive(visible);
            }
        }

        public void Clear()
        {
            foreach (var e in _entries)
                if (e.Root != null) UnityEngine.Object.Destroy(e.Root);
            _entries.Clear();
            if (_attached)
            {
                _frame.Remove(_follower);
                _attached = false;
            }
        }

        public void Dispose() => Clear();

        void EnsureFollower()
        {
            if (_attached) return;
            _frame.Add(_follower);
            _attached = true;
        }

        void FollowTargets()
        {
            for (int i = _entries.Count - 1; i >= 0; --i)
            {
                var e = _entries[i];
                if (e.Target == null)
                {
                    if (e.Root != null) UnityEngine.Object.Destroy(e.Root);
                    _entries.RemoveAt(i);
                    continue;
                }
                if (e.Root == null) { _entries.RemoveAt(i); continue; }
                e.Root.transform.position = e.Target.position;
            }
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
