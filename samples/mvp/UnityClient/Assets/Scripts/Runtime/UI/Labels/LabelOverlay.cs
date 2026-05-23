using System;
using System.Collections.Generic;
using Atlas.Client.Unity;
using UnityEngine;
using UnityEngine.UI;

namespace Atlas.Mvp.Unity
{
    public sealed class LabelOverlay : IAtlasUnityTickable, IDisposable
    {
        readonly AtlasUnityFramePump _frame;
        readonly Func<Camera?> _cameraSource;
        readonly Canvas _canvas;
        readonly Font _font;
        readonly List<DamageFloater> _floaters = new();
        readonly List<DamageFloater> _expired = new();
        Camera? _camera;

        public LabelOverlay(AtlasUnityFramePump frame, Func<Camera?> cameraSource)
        {
            _frame = frame ?? throw new ArgumentNullException(nameof(frame));
            _cameraSource = cameraSource ?? throw new ArgumentNullException(nameof(cameraSource));
            _font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");

            var go = new GameObject("LabelCanvas");
            _canvas = go.AddComponent<Canvas>();
            _canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            _canvas.sortingOrder = 100;
            var scaler = go.AddComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ConstantPixelSize;
            scaler.scaleFactor = 1f;
            _frame.Add(this);
        }

        public void Dispose()
        {
            _frame.Remove(this);
            foreach (var f in _floaters) f.DestroyText();
            _floaters.Clear();
            if (_canvas != null) UnityEngine.Object.Destroy(_canvas.gameObject);
        }

        public Text CreateLabel()
        {
            var go = new GameObject("Label");
            go.transform.SetParent(_canvas.transform, false);
            var text = go.AddComponent<Text>();
            text.font = _font;
            text.fontSize = 14;
            text.alignment = TextAnchor.LowerCenter;
            text.color = Color.white;
            text.horizontalOverflow = HorizontalWrapMode.Overflow;
            text.verticalOverflow = VerticalWrapMode.Overflow;
            text.raycastTarget = false;
            var rt = text.rectTransform;
            rt.anchorMin = rt.anchorMax = Vector2.zero;
            rt.pivot = new Vector2(0.5f, 0f);
            rt.sizeDelta = new Vector2(220f, 40f);
            return text;
        }

        public bool TryProject(Vector3 worldPos, out Vector3 screenPos)
        {
            if (_camera == null) _camera = _cameraSource() ?? Camera.main;
            if (_camera == null) { screenPos = default; return false; }
            screenPos = _camera.WorldToScreenPoint(worldPos);
            return screenPos.z > 0f;
        }

        public void RegisterFloater(DamageFloater f) => _floaters.Add(f);

        public void Tick(float dt)
        {
            _expired.Clear();
            for (int i = 0; i < _floaters.Count; ++i)
            {
                if (!_floaters[i].Advance(dt)) _expired.Add(_floaters[i]);
            }
            foreach (var f in _expired)
            {
                f.DestroyText();
                _floaters.Remove(f);
            }
        }
    }
}
