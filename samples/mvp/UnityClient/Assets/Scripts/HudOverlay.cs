using Atlas.Client.Unity;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public sealed class HudOverlay : MonoBehaviour
    {
        AtlasNetworkManager _net = null!;
        float _fps;
        uint _rttMs;
        float _statsAccum;
        GUIStyle? _style;
        Texture2D? _bg;

        public void Bind(AtlasNetworkManager net) => _net = net;

        void Update()
        {
            float dt = Time.unscaledDeltaTime;
            if (dt > 0)
            {
                float instant = 1f / dt;
                _fps = _fps > 0 ? _fps * 0.9f + instant * 0.1f : instant;
            }
            _statsAccum += dt;
            if (_statsAccum >= 0.5f)
            {
                _statsAccum = 0f;
                if (_net != null && _net.TryGetStats(out var s)) _rttMs = s.RttMs;
            }
        }

        void OnGUI()
        {
            if (_style == null)
            {
                _style = new GUIStyle(GUI.skin.label) { fontSize = 16, alignment = TextAnchor.UpperLeft };
                _style.normal.textColor = Color.white;
            }
            if (_bg == null)
            {
                _bg = new Texture2D(1, 1);
                _bg.SetPixel(0, 0, new Color(0f, 0f, 0f, 0.45f));
                _bg.Apply();
            }
            const float w = 150f;
            const float h = 74f;
            var rect = new Rect(Screen.width - w - 6f, 6f, w, h);
            GUI.DrawTexture(rect, _bg);
            var inner = new Rect(rect.x + 8f, rect.y + 4f, rect.width - 16f, rect.height - 8f);
            GUI.Label(inner, $"fps  {_fps:F0}\nping {_rttMs}ms", _style);
            var toggleRect = new Rect(inner.x, inner.y + 44f, inner.width, 22f);
            bool showAoi = GUI.Toggle(toggleRect, AoIDebugRing.Visible, "show AoI");
            if (showAoi != AoIDebugRing.Visible) AoIDebugRing.SetVisible(showAoi);
        }
    }
}
