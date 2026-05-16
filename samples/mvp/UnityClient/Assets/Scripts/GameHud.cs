using System;
using Atlas.Client.Unity;
using UnityEngine;
using UnityEngine.UIElements;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

namespace Atlas.Mvp.Unity
{
    [RequireComponent(typeof(UIDocument))]
    public sealed class GameHud : MonoBehaviour
    {
        public event Action? LogoutRequested;

        UIDocument _doc = null!;
        AtlasNetworkManager? _net;
        MvpAvatar? _owner;
        int _observedMaxHp = 100;

        VisualElement _hpFill = null!;
        Label _hpName = null!;
        Label _hpText = null!;
        Label _fpsValue = null!;
        Label _pingValue = null!;
        VisualElement _aoiToggle = null!;
        VisualElement _aoiCheckbox = null!;
        Button _logoutButton = null!;

        float _fps;
        uint _rttMs;
        float _statsAccum;

        public void Bind(AtlasNetworkManager net) => _net = net;

        public void SetOwner(MvpAvatar? avatar)
        {
            _owner = avatar;
            if (_owner != null)
            {
                _observedMaxHp = Mathf.Max(_observedMaxHp, _owner.Hp);
                _hpName.text = $"Avatar #{_owner.EntityId}";
            }
            else
            {
                _hpName.text = "—";
            }
            RefreshHp();
        }

        void Awake()
        {
            _doc = GetComponent<UIDocument>();
            EnsurePanelSettings();
            EnsureVisualTree();

            var root = _doc.rootVisualElement;
            _hpFill = root.Q<VisualElement>("hp-bar-fill");
            _hpName = root.Q<Label>("hp-name");
            _hpText = root.Q<Label>("hp-text");
            _fpsValue = root.Q<Label>("fps-value");
            _pingValue = root.Q<Label>("ping-value");
            _aoiToggle = root.Q<VisualElement>("aoi-toggle");
            _aoiCheckbox = root.Q<VisualElement>("aoi-checkbox");
            _logoutButton = root.Q<Button>("logout-button");

            if (_hpFill == null || _hpName == null || _hpText == null
                || _fpsValue == null || _pingValue == null
                || _aoiToggle == null || _aoiCheckbox == null || _logoutButton == null)
            {
                Debug.LogError("[GameHud] UXML missing expected names — check GameHud.uxml");
                return;
            }

            _aoiToggle.focusable = false;
            _aoiToggle.RegisterCallback<ClickEvent>(_ =>
            {
                AoIDebugRing.SetVisible(!AoIDebugRing.Visible);
                ApplyAoiToggleCheckedStyle(AoIDebugRing.Visible);
            });
            ApplyAoiToggleCheckedStyle(AoIDebugRing.Visible);

            _logoutButton.focusable = false;
            _logoutButton.clicked += () => LogoutRequested?.Invoke();
            RefreshHp();
        }

        void Update()
        {
            float dt = Time.unscaledDeltaTime;
            if (dt > 0f)
            {
                float instant = 1f / dt;
                _fps = _fps > 0f ? _fps * 0.9f + instant * 0.1f : instant;
            }
            _fpsValue.text = _fps.ToString("F0");

            _statsAccum += dt;
            if (_statsAccum >= 0.5f)
            {
                _statsAccum = 0f;
                if (_net != null && _net.TryGetStats(out var s))
                {
                    _rttMs = s.RttMs;
                    _pingValue.text = $"{_rttMs} ms";
                }
            }

            if (_owner != null && !_owner.IsDestroyed)
                RefreshHp();
        }

        void RefreshHp()
        {
            if (_owner == null || _owner.IsDestroyed)
            {
                _hpText.text = "HP --/--";
                _hpFill.style.width = new Length(0f, LengthUnit.Percent);
                ApplyHpClass(0f, dead: true);
                return;
            }
            int hp = Mathf.Max(0, _owner.Hp);
            if (hp > _observedMaxHp) _observedMaxHp = hp;
            float pct = _observedMaxHp > 0 ? (float)hp / _observedMaxHp : 0f;
            pct = Mathf.Clamp01(pct);
            _hpText.text = $"HP {hp}/{_observedMaxHp}";
            _hpFill.style.width = new Length(pct * 100f, LengthUnit.Percent);
            ApplyHpClass(pct, dead: _owner.IsDead);
        }

        void ApplyAoiToggleCheckedStyle(bool isChecked)
        {
            _aoiCheckbox.EnableInClassList("checked", isChecked);
        }

        void ApplyHpClass(float pct, bool dead)
        {
            _hpFill.RemoveFromClassList("warn");
            _hpFill.RemoveFromClassList("danger");
            _hpFill.RemoveFromClassList("dead");
            if (dead) _hpFill.AddToClassList("dead");
            else if (pct < 0.25f) _hpFill.AddToClassList("danger");
            else if (pct < 0.5f) _hpFill.AddToClassList("warn");
        }

        void EnsurePanelSettings()
        {
            if (_doc.panelSettings != null) return;
            var settings = Resources.Load<PanelSettings>("UI/LoginPanelSettings");
            if (settings == null)
            {
                Debug.LogError(
                    "[GameHud] PanelSettings missing — reuse LoginPanelSettings.asset");
                return;
            }
            _doc.panelSettings = settings;
        }

        void EnsureVisualTree()
        {
            if (_doc.visualTreeAsset != null) return;
            var tree = Resources.Load<VisualTreeAsset>("UI/GameHud");
            if (tree == null)
            {
                Debug.LogError("[GameHud] GameHud.uxml missing under Assets/Resources/UI/");
                return;
            }
            _doc.visualTreeAsset = tree;
        }
    }
}
