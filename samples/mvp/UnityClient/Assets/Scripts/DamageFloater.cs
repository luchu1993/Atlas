using UnityEngine;
using UnityEngine.UI;

namespace Atlas.Mvp.Unity
{
    public sealed class DamageFloater : MonoBehaviour
    {
        const float kLifetimeSec = 1.0f;
        const float kRisePixelsPerSec = 90f;

        float _age;
        Vector2 _anchored;
        Text _text = null!;

        public static void Spawn(Vector3 worldPos, int amount)
        {
            var overlay = LabelOverlay.Instance;
            if (overlay == null) return;
            if (!overlay.TryProject(worldPos, out var screen)) return;

            var text = overlay.CreateLabel();
            text.text = $"-{amount}";
            text.color = Color.red;
            text.fontSize = 24;
            text.fontStyle = FontStyle.Bold;
            text.rectTransform.pivot = new Vector2(0.5f, 0.5f);
            var anchored = new Vector2(screen.x, screen.y);
            text.rectTransform.anchoredPosition = anchored;

            var floater = text.gameObject.AddComponent<DamageFloater>();
            floater._text = text;
            floater._anchored = anchored;
        }

        void Update()
        {
            _age += Time.deltaTime;
            _anchored.y += kRisePixelsPerSec * Time.deltaTime;
            _text.rectTransform.anchoredPosition = _anchored;
            var alpha = Mathf.Clamp01(1f - _age / kLifetimeSec);
            var c = _text.color; c.a = alpha; _text.color = c;
            if (_age >= kLifetimeSec) Destroy(gameObject);
        }
    }
}
