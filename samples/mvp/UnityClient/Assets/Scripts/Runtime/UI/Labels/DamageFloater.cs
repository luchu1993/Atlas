using UnityEngine;
using UnityEngine.UI;

namespace Atlas.Mvp.Unity
{
    public sealed class DamageFloater
    {
        const float kLifetimeSec = 1.0f;
        const float kRisePixelsPerSec = 90f;

        float _age;
        Vector2 _anchored;
        readonly Text _text;

        DamageFloater(Text text, Vector2 anchored)
        {
            _text = text;
            _anchored = anchored;
        }

        public static void Spawn(LabelOverlay overlay, Vector3 worldPos, int amount)
        {
            if (!overlay.TryProject(worldPos, out var screen)) return;

            var text = overlay.CreateLabel();
            text.text = $"-{amount}";
            text.color = Color.red;
            text.fontSize = 24;
            text.fontStyle = FontStyle.Bold;
            text.rectTransform.pivot = new Vector2(0.5f, 0.5f);
            var anchored = new Vector2(screen.x, screen.y);
            text.rectTransform.anchoredPosition = anchored;

            overlay.RegisterFloater(new DamageFloater(text, anchored));
        }

        public bool Advance(float dt)
        {
            _age += dt;
            _anchored.y += kRisePixelsPerSec * dt;
            _text.rectTransform.anchoredPosition = _anchored;
            var alpha = Mathf.Clamp01(1f - _age / kLifetimeSec);
            var c = _text.color; c.a = alpha; _text.color = c;
            return _age < kLifetimeSec;
        }

        public void DestroyText()
        {
            if (_text != null) Object.Destroy(_text.gameObject);
        }
    }
}
