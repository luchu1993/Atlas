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
    public sealed partial class GameHud : MonoBehaviour
    {
        public event Action? LogoutRequested;

        const float kJoystickZoneSize = 200f;
        const float kJoystickKnobSize = 56f;
        const float kJoystickKnobRest = (kJoystickZoneSize - kJoystickKnobSize) * 0.5f;
        const float kJoystickMaxRadius = 80f;

        UIDocument _doc = null!;
        AtlasNetworkManager? _net;
        ViewRegistry? _views;
        AoiBoxOverlay? _aoiBoxes;
        BspGizmo? _bspGizmo;
        MvpAvatar? _owner;
        Transform? _ownerTransform;
        int _observedMaxHp = 100;
        // MVP is a single space; world bootstrap pins kSpaceId = 1.
        const uint kSpaceId = 1;

        VisualElement _hpFill = null!;
        Label _hpName = null!;
        Label _hpText = null!;
        Label _fpsValue = null!;
        Label _pingValue = null!;
        Label _npcAllValue = null!;
        Label _npcAoiValue = null!;
        Label _levelValue = null!;
        Label _goldValue = null!;
        Label _sessionValue = null!;
        Label _weaponValue = null!;
        Label _bwUpValue = null!;
        Label _bwDownValue = null!;
        Label _queueValue = null!;
        Label _rpcOutValue = null!;
        Label _aoiEnterValue = null!;
        Label _aoiLeaveValue = null!;
        Label _cellValue = null!;
        Label _crossValue = null!;
        VisualElement _aoiToggle = null!;
        VisualElement _aoiCheckbox = null!;
        VisualElement _bspToggle = null!;
        VisualElement _bspCheckbox = null!;
        int _lastQuadrant;  // 0 = uninit, otherwise 1..4 matching ComputeQuadrant
        uint _crossCount;
        Button _logoutButton = null!;
        VisualElement _debugPanel = null!;
        ScrollView _chatScroll = null!;
        TextField _chatInput = null!;
        const int kChatScrollback = 5;
        bool _chatFocused;
        bool _chatUserInteracted;
        public bool IsChatFocused => _chatFocused;
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
        uint _lastBytesSent;
        uint _lastBytesRecv;
        uint _lastRpcOut;
        uint _lastAoiEnter;
        uint _lastAoiLeave;
        bool _ratesPrimed;

        public void Bind(AtlasNetworkManager net) => _net = net;

        public void BindViewRegistry(ViewRegistry views) => _views = views;

        public void SetOwner(MvpAvatar? avatar, Transform? avatarTransform = null)
        {
            _ownerTransform = avatarTransform;
            _lastQuadrant = 0;
            _crossCount = 0;
            if (_crossValue != null) _crossValue.text = "0";
            if (_cellValue != null) _cellValue.text = "--";
            if (_owner != null)
            {
                _owner.GoldChanged -= OnOwnerGoldChanged;
                _owner.LevelChanged -= OnOwnerLevelChanged;
                _owner.SessionInfoReceived -= OnOwnerSessionInfo;
            }
            _owner = avatar;
            if (_owner != null)
            {
                _observedMaxHp = Mathf.Max(_observedMaxHp, _owner.Hp);
                _hpName.text = $"AVATAR #{_owner.EntityId}";
                _owner.GoldChanged += OnOwnerGoldChanged;
                _owner.LevelChanged += OnOwnerLevelChanged;
                _owner.SessionInfoReceived += OnOwnerSessionInfo;
                OnOwnerGoldChanged(_owner.Gold);
                OnOwnerLevelChanged(_owner.Level);
                OnOwnerSessionInfo(_owner.LastSessionLabel);  // replay if RPC arrived pre-subscribe
                _weaponValue.text = WeaponName(_owner.Equipment?.WeaponId ?? 0);
            }
            else
            {
                _hpName.text = "—";
                _goldValue.text = "--";
                _levelValue.text = "--";
                _weaponValue.text = "--";
                _sessionValue.text = "--";
            }
            RefreshHp();
        }

        void OnOwnerSessionInfo(string label) =>
            _sessionValue.text = string.IsNullOrEmpty(label) ? "--" : label;

        void OnOwnerGoldChanged(int newGold) => _goldValue.text = newGold.ToString();
        void OnOwnerLevelChanged(int newLevel) => _levelValue.text = newLevel.ToString();

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
            _npcAllValue = root.Q<Label>("npc-all-value");
            _npcAoiValue = root.Q<Label>("npc-aoi-value");
            _levelValue = root.Q<Label>("level-value");
            _goldValue = root.Q<Label>("gold-value");
            _sessionValue = root.Q<Label>("session-value");
            _weaponValue = root.Q<Label>("weapon-value");
            _bwUpValue = root.Q<Label>("bw-up-value");
            _bwDownValue = root.Q<Label>("bw-down-value");
            _queueValue = root.Q<Label>("queue-value");
            _rpcOutValue = root.Q<Label>("rpc-out-value");
            _aoiEnterValue = root.Q<Label>("aoi-enter-value");
            _aoiLeaveValue = root.Q<Label>("aoi-leave-value");
            _cellValue = root.Q<Label>("cell-value");
            _crossValue = root.Q<Label>("cross-value");
            _aoiToggle = root.Q<VisualElement>("aoi-toggle");
            _aoiCheckbox = root.Q<VisualElement>("aoi-checkbox");
            _bspToggle = root.Q<VisualElement>("bsp-toggle");
            _bspCheckbox = root.Q<VisualElement>("bsp-checkbox");
            _logoutButton = root.Q<Button>("logout-button");
            _debugPanel = root.Q<VisualElement>("debug-panel");
            _chatScroll = root.Q<ScrollView>("chat-scroll");
            _chatInput = root.Q<TextField>("chat-input");
            _joystick = root.Q<VisualElement>("joystick");
            _joystickBase = root.Q<VisualElement>("joystick-base");
            _joystickKnob = root.Q<VisualElement>("joystick-knob");
            _skillButton = root.Q<VisualElement>("skill-button");
            _lookZone = root.Q<VisualElement>("look-zone");

            if (_hpFill == null || _hpName == null || _hpText == null
                || _fpsValue == null || _pingValue == null
                || _npcAllValue == null || _npcAoiValue == null
                || _levelValue == null || _goldValue == null || _sessionValue == null
                || _weaponValue == null
                || _bwUpValue == null || _bwDownValue == null
                || _queueValue == null || _rpcOutValue == null
                || _aoiEnterValue == null || _aoiLeaveValue == null
                || _cellValue == null || _crossValue == null
                || _aoiToggle == null || _aoiCheckbox == null
                || _bspToggle == null || _bspCheckbox == null
                || _logoutButton == null || _debugPanel == null
                || _chatScroll == null || _chatInput == null
                || _joystick == null || _joystickBase == null || _joystickKnob == null
                || _skillButton == null || _lookZone == null)
            {
                Debug.LogError("[GameHud] UXML missing expected names — check GameHud.uxml");
                return;
            }

            _aoiToggle.focusable = false;
            _aoiToggle.RegisterCallback<ClickEvent>(_ =>
            {
                if (_aoiBoxes == null) return;
                _aoiBoxes.SetVisible(!_aoiBoxes.Visible);
                _aoiCheckbox.EnableInClassList("checked", _aoiBoxes.Visible);
            });
            _aoiCheckbox.EnableInClassList("checked", _aoiBoxes?.Visible ?? false);

            _bspToggle.focusable = false;
            _bspToggle.RegisterCallback<ClickEvent>(_ =>
            {
                if (_bspGizmo == null) return;
                _bspGizmo.SetVisible(!_bspGizmo.Visible);
                _bspCheckbox.EnableInClassList("checked", _bspGizmo.Visible);
            });
            _bspCheckbox.EnableInClassList("checked", _bspGizmo?.Visible ?? false);

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

            // UIToolkit auto-focuses the first focusable on panel attach; ignore
            // that initial focus and only honour real user pointer interaction.
            _chatInput.RegisterCallback<PointerDownEvent>(_ => _chatUserInteracted = true);
            _chatInput.RegisterCallback<FocusInEvent>(_ => {
                if (_chatUserInteracted) _chatFocused = true;
                else _chatInput.Blur();
            });
            _chatInput.RegisterCallback<FocusOutEvent>(_ => {
                _chatFocused = false;
                _chatUserInteracted = false;
            });
            // FocusOut only fires when focus moves to another focusable; clicks
            // on look-zone/joystick leave the TextField focused and WASD blocked.
            root.RegisterCallback<PointerDownEvent>(OnRootPointerDown, TrickleDown.TrickleDown);
            // TrickleDown so Return/Esc are caught before the TextField's
            // internal handlers consume them (multi-line submit + clear).
            _chatInput.RegisterCallback<KeyDownEvent>(OnChatKeyDown, TrickleDown.TrickleDown);
            Atlas.Mvp.Client.ChatBus.Received += OnChatBusReceived;
            Atlas.Components.EquipmentComponent.WeaponChanged += OnWeaponChanged;

            RefreshHp();
        }

        void OnDestroy()
        {
            Atlas.Mvp.Client.ChatBus.Received -= OnChatBusReceived;
            Atlas.Components.EquipmentComponent.WeaponChanged -= OnWeaponChanged;
        }

        static string WeaponName(int id) => id switch
        {
            1 => "Sword",
            2 => "Bow",
            3 => "Staff",
            _ => "(none)",
        };

        void OnWeaponChanged(uint entityId, int weaponId)
        {
            if (_owner == null || entityId != _owner.EntityId) return;
            _weaponValue.text = WeaponName(weaponId);
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
                UpdateNetStats(_statsAccum);
                _statsAccum = 0f;
                int npcAoi = _views?.NpcViewCount ?? 0;
                int npcAll = _net?.Session.SpaceDataManager
                    .GetInt32(kSpaceId, SpaceDataKeys.NpcCount) ?? 0;
                _npcAllValue.text = npcAll.ToString();
                _npcAoiValue.text = npcAoi.ToString();
            }

            if (_owner != null && !_owner.IsDestroyed)
                RefreshHp();

            RefreshCellIndicator();
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
