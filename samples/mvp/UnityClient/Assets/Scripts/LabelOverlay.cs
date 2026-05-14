using UnityEngine;
using UnityEngine.UI;

namespace Atlas.Mvp.Unity
{
    public sealed class LabelOverlay : MonoBehaviour
    {
        public static LabelOverlay? Instance { get; private set; }

        Camera _camera = null!;
        Canvas _canvas = null!;
        Font _font = null!;

        void Awake()
        {
            Instance = this;
            _font = Resources.GetBuiltinResource<Font>("LegacyRuntime.ttf");

            // Root-level canvas: ScreenSpaceOverlay auto-sizes to screen pixels,
            // so child.transform.position == WorldToScreenPoint output directly.
            var go = new GameObject("LabelCanvas");
            _canvas = go.AddComponent<Canvas>();
            _canvas.renderMode = RenderMode.ScreenSpaceOverlay;
            _canvas.sortingOrder = 100;
            var scaler = go.AddComponent<CanvasScaler>();
            scaler.uiScaleMode = CanvasScaler.ScaleMode.ConstantPixelSize;
            scaler.scaleFactor = 1f;
        }

        void OnDestroy()
        {
            if (Instance == this) Instance = null;
            if (_canvas != null) Destroy(_canvas.gameObject);
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
            // Anchor at canvas (0,0) so anchoredPosition == screen pixels;
            // bottom-center pivot grows the label upward from the projected head.
            var rt = text.rectTransform;
            rt.anchorMin = rt.anchorMax = Vector2.zero;
            rt.pivot = new Vector2(0.5f, 0f);
            rt.sizeDelta = new Vector2(220f, 40f);
            return text;
        }

        // Prefers Bootstrap.MainCamera: a stray scene-template Main Camera
        // confuses Camera.main and projects labels to garbage coords.
        public bool TryProject(Vector3 worldPos, out Vector3 screenPos)
        {
            if (_camera == null) _camera = Bootstrap.Instance?.MainCamera ?? Camera.main;
            if (_camera == null) { screenPos = default; return false; }
            screenPos = _camera.WorldToScreenPoint(worldPos);
            return screenPos.z > 0f;
        }
    }
}
