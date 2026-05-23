using System;
using UnityEngine;
using UnityEngine.UIElements;

namespace Atlas.Mvp.Unity
{
    [RequireComponent(typeof(UIDocument))]
    public sealed class LoginScreen : MonoBehaviour
    {
        public event Action<string, ushort, string, string>? LoginRequested;

        UIDocument _doc = null!;
        TextField _hostField = null!;
        TextField _portField = null!;
        TextField _userField = null!;
        TextField _passField = null!;
        Button _loginButton = null!;
        Label _statusLabel = null!;
        VisualElement _overlay = null!;

        public void Configure(string host, ushort port, string username, string passwordHash)
        {
            _hostField.value = host;
            _portField.value = port.ToString();
            _userField.value = username;
            _passField.value = passwordHash;
        }

        public void SetStatus(string text, bool isError)
        {
            _statusLabel.text = text;
            if (isError) _statusLabel.AddToClassList("error");
            else _statusLabel.RemoveFromClassList("error");
        }

        public void SetInteractable(bool enabled)
        {
            _hostField.SetEnabled(enabled);
            _portField.SetEnabled(enabled);
            _userField.SetEnabled(enabled);
            _passField.SetEnabled(enabled);
            _loginButton.SetEnabled(enabled);
        }

        public void Show() => _overlay.style.display = DisplayStyle.Flex;
        public void Hide() => _overlay.style.display = DisplayStyle.None;

        void Awake()
        {
            _doc = GetComponent<UIDocument>();
            EnsurePanelSettings();
            EnsureVisualTree();

            var root = _doc.rootVisualElement;
            _overlay = root.Q<VisualElement>("overlay");
            _hostField = root.Q<TextField>("host-field");
            _portField = root.Q<TextField>("port-field");
            _userField = root.Q<TextField>("username-field");
            _passField = root.Q<TextField>("password-field");
            _loginButton = root.Q<Button>("login-button");
            _statusLabel = root.Q<Label>("status-label");

            if (_overlay == null || _hostField == null || _portField == null
                || _userField == null || _passField == null
                || _loginButton == null || _statusLabel == null)
            {
                Debug.LogError(
                    "[LoginScreen] UXML missing expected element names — check LoginScreen.uxml");
                return;
            }

            _loginButton.clicked += OnLoginClicked;
        }

        void EnsurePanelSettings()
        {
            if (_doc.panelSettings != null) return;
            var settings = Resources.Load<PanelSettings>("UI/LoginPanelSettings");
            if (settings == null)
            {
                Debug.LogError(
                    "[LoginScreen] PanelSettings asset missing — create "
                    + "Assets/Resources/UI/LoginPanelSettings.asset via "
                    + "Create → UI Toolkit → Panel Settings Asset");
                return;
            }
            _doc.panelSettings = settings;
        }

        void EnsureVisualTree()
        {
            if (_doc.visualTreeAsset != null) return;
            var tree = Resources.Load<VisualTreeAsset>("UI/LoginScreen");
            if (tree == null)
            {
                Debug.LogError(
                    "[LoginScreen] LoginScreen.uxml missing under Assets/Resources/UI/");
                return;
            }
            _doc.visualTreeAsset = tree;
        }

        void OnLoginClicked()
        {
            if (!ushort.TryParse(_portField.value, out ushort port))
            {
                SetStatus("Port must be a number 1–65535", isError: true);
                return;
            }
            string host = _hostField.value.Trim();
            string user = _userField.value.Trim();
            string pwd = _passField.value;
            if (host.Length == 0 || user.Length == 0 || pwd.Length == 0)
            {
                SetStatus("Host / username / password required", isError: true);
                return;
            }
            LoginRequested?.Invoke(host, port, user, pwd);
        }
    }
}
