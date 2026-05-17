using System;
using Atlas.Client;
using Atlas.Client.Unity;
using Atlas.Space;
using UnityEngine;
using UnityEngine.UIElements;
using MvpAvatar = Atlas.Mvp.Client.Avatar;

namespace Atlas.Mvp.Unity
{
    [RequireComponent(typeof(UIDocument))]
    public sealed class GameHud : MonoBehaviour
    {
        public event Action? LogoutRequested;

        const float kJoystickZoneSize = 200f;
        const float kJoystickKnobSize = 56f;
        const float kJoystickKnobRest = (kJoystickZoneSize - kJoystickKnobSize) * 0.5f;
        const float kJoystickMaxRadius = 80f;

        UIDocument _doc = null!;
        AtlasNetworkManager? _net;
        ViewRegistry? _views;
        MvpAvatar? _owner;
        int _observedMaxHp = 100;
        // MVP is a single space; world bootstrap pins kSpaceId = 1.
        const uint kSpaceId = 1;

        VisualElement _hpFill = null!;
        Label _hpName = null!;
        Label _hpText = null!;
        Label _fpsValue = null!;
        Label _pingValue = null!;
        Label _npcAoiValue = null!;
        Label _npcAllValue = null!;
        VisualElement _aoiToggle = null!;
        VisualElement _aoiCheckbox = null!;
        Button _logoutButton = null!;
        VisualElement _debugPanel = null!;
        VisualElement _joystick = null!;
        VisualElement _joystickBase = null!;
        VisualElement _joystickKnob = null!;
        int _joystickPointerId = -1;
        Vector2 _joystickInput;
        VisualElement _skillButton = null!;
        bool _firePending;
        VisualElement _lookZone = null!;
        int _lookPointerId = -1;
        Vector2 _lookLastPos;
        Vector2 _lookDeltaAccum;

        public Vector2 JoystickInput => _joystickInput;

        public bool ConsumeFireRequest()
        {
            if (!_firePending) return false;
            _firePending = false;
            return true;
        }

        public Vector2 ConsumeLookDelta()
        {
            var d = _lookDeltaAccum;
            _lookDeltaAccum = Vector2.zero;
            return d;
        }

        float _fps;
        uint _rttMs;
        float _statsAccum;

        public void Bind(AtlasNetworkManager net) => _net = net;

        public void BindViewRegistry(ViewRegistry views) => _views = views;

        public void SetOwner(MvpAvatar? avatar)
        {
            _owner = avatar;
            if (_owner != null)
            {
                _observedMaxHp = Mathf.Max(_observedMaxHp, _owner.Hp);
                _hpName.text = $"AVATAR #{_owner.EntityId}";
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
            _npcAoiValue = root.Q<Label>("npc-aoi-value");
            _npcAllValue = root.Q<Label>("npc-all-value");
            _aoiToggle = root.Q<VisualElement>("aoi-toggle");
            _aoiCheckbox = root.Q<VisualElement>("aoi-checkbox");
            _logoutButton = root.Q<Button>("logout-button");
            _debugPanel = root.Q<VisualElement>("debug-panel");
            _joystick = root.Q<VisualElement>("joystick");
            _joystickBase = root.Q<VisualElement>("joystick-base");
            _joystickKnob = root.Q<VisualElement>("joystick-knob");
            _skillButton = root.Q<VisualElement>("skill-button");
            _lookZone = root.Q<VisualElement>("look-zone");

            if (_hpFill == null || _hpName == null || _hpText == null
                || _fpsValue == null || _pingValue == null
                || _npcAoiValue == null || _npcAllValue == null
                || _aoiToggle == null || _aoiCheckbox == null
                || _logoutButton == null || _debugPanel == null
                || _joystick == null || _joystickBase == null || _joystickKnob == null
                || _skillButton == null || _lookZone == null)
            {
                Debug.LogError("[GameHud] UXML missing expected names — check GameHud.uxml");
                return;
            }

            _aoiToggle.focusable = false;
            _aoiToggle.RegisterCallback<ClickEvent>(_ =>
            {
                AoiBoxes.SetVisible(!AoiBoxes.Visible);
                ApplyAoiToggleCheckedStyle(AoiBoxes.Visible);
            });
            ApplyAoiToggleCheckedStyle(AoiBoxes.Visible);

            _logoutButton.focusable = false;
            _logoutButton.clicked += () => LogoutRequested?.Invoke();

            _joystick.RegisterCallback<PointerDownEvent>(OnJoystickDown);
            _joystick.RegisterCallback<PointerMoveEvent>(OnJoystickMove);
            _joystick.RegisterCallback<PointerUpEvent>(OnJoystickUp);
            _joystick.RegisterCallback<PointerCancelEvent>(_ => ReleaseJoystick());
            _joystick.RegisterCallback<PointerCaptureOutEvent>(_ => ReleaseJoystick());

            _skillButton.RegisterCallback<PointerDownEvent>(evt =>
            {
                _firePending = true;
                _skillButton.AddToClassList("active");
                evt.StopPropagation();
            });
            _skillButton.RegisterCallback<PointerUpEvent>(_ =>
                _skillButton.RemoveFromClassList("active"));
            _skillButton.RegisterCallback<PointerLeaveEvent>(_ =>
                _skillButton.RemoveFromClassList("active"));

            _lookZone.RegisterCallback<PointerDownEvent>(OnLookDown);
            _lookZone.RegisterCallback<PointerMoveEvent>(OnLookMove);
            _lookZone.RegisterCallback<PointerUpEvent>(OnLookUp);
            _lookZone.RegisterCallback<PointerCancelEvent>(_ => ReleaseLook());
            _lookZone.RegisterCallback<PointerCaptureOutEvent>(_ => ReleaseLook());

            RefreshHp();
        }

        void OnLookDown(PointerDownEvent evt)
        {
            // Right-click is handled by Bootstrap's legacy Input path so the
            // desktop "hold RMB to orbit" gesture keeps working as-is.
            if (evt.button != 0) return;
            if (_lookPointerId != -1) return;
            _lookPointerId = evt.pointerId;
            _lookZone.CapturePointer(evt.pointerId);
            _lookLastPos = (Vector2)evt.position;
            evt.StopPropagation();
        }

        void OnLookMove(PointerMoveEvent evt)
        {
            if (evt.pointerId != _lookPointerId) return;
            var pos = (Vector2)evt.position;
            _lookDeltaAccum += pos - _lookLastPos;
            _lookLastPos = pos;
            evt.StopPropagation();
        }

        void OnLookUp(PointerUpEvent evt)
        {
            if (evt.pointerId != _lookPointerId) return;
            ReleaseLook();
            evt.StopPropagation();
        }

        void ReleaseLook()
        {
            if (_lookPointerId != -1 && _lookZone.HasPointerCapture(_lookPointerId))
                _lookZone.ReleasePointer(_lookPointerId);
            _lookPointerId = -1;
        }

        void OnJoystickDown(PointerDownEvent evt)
        {
            if (_joystickPointerId != -1) return;
            _joystickPointerId = evt.pointerId;
            _joystick.CapturePointer(evt.pointerId);
            _joystickBase.AddToClassList("active");
            _joystickKnob.AddToClassList("active");
            UpdateJoystick(evt.localPosition);
            evt.StopPropagation();
        }

        void OnJoystickMove(PointerMoveEvent evt)
        {
            if (evt.pointerId != _joystickPointerId) return;
            UpdateJoystick(evt.localPosition);
            evt.StopPropagation();
        }

        void OnJoystickUp(PointerUpEvent evt)
        {
            if (evt.pointerId != _joystickPointerId) return;
            ReleaseJoystick();
            evt.StopPropagation();
        }

        void UpdateJoystick(Vector3 localPos)
        {
            float dx = localPos.x - kJoystickZoneSize * 0.5f;
            float dy = localPos.y - kJoystickZoneSize * 0.5f;
            var d = new Vector2(dx, dy);
            if (d.magnitude > kJoystickMaxRadius)
                d = d.normalized * kJoystickMaxRadius;
            _joystickKnob.style.left = new Length(kJoystickKnobRest + d.x, LengthUnit.Pixel);
            _joystickKnob.style.top = new Length(kJoystickKnobRest + d.y, LengthUnit.Pixel);
            // UI Y grows downward; flip so up-on-screen maps to forward (+z).
            _joystickInput = new Vector2(d.x / kJoystickMaxRadius, -d.y / kJoystickMaxRadius);
        }

        void ReleaseJoystick()
        {
            if (_joystickPointerId != -1 && _joystick.HasPointerCapture(_joystickPointerId))
                _joystick.ReleasePointer(_joystickPointerId);
            _joystickPointerId = -1;
            _joystickKnob.style.left = new Length(kJoystickKnobRest, LengthUnit.Pixel);
            _joystickKnob.style.top = new Length(kJoystickKnobRest, LengthUnit.Pixel);
            _joystickInput = Vector2.zero;
            _joystickBase.RemoveFromClassList("active");
            _joystickKnob.RemoveFromClassList("active");
        }

        void Update()
        {
            if (Input.GetKeyDown(KeyCode.F1))
            {
                bool open = _debugPanel.style.display == DisplayStyle.Flex;
                _debugPanel.style.display = open ? DisplayStyle.None : DisplayStyle.Flex;
            }

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
                    _pingValue.text = $"{_rttMs} MS";
                }
                _npcAoiValue.text = (_views?.NpcViewCount ?? 0).ToString();
                _npcAllValue.text = ClientCallbacks.SpaceDataManager
                    .GetInt32(kSpaceId, SpaceDataKeys.NpcCount).ToString();
            }

            if (_owner != null && !_owner.IsDestroyed)
                RefreshHp();
        }

        void RefreshHp()
        {
            if (_owner == null || _owner.IsDestroyed)
            {
                _hpText.text = "--/--";
                _hpFill.style.width = new Length(0f, LengthUnit.Percent);
                ApplyHpClass(0f, dead: true);
                return;
            }
            int hp = Mathf.Max(0, _owner.Hp);
            if (hp > _observedMaxHp) _observedMaxHp = hp;
            float pct = _observedMaxHp > 0 ? (float)hp / _observedMaxHp : 0f;
            pct = Mathf.Clamp01(pct);
            _hpText.text = $"{hp} / {_observedMaxHp}";
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
