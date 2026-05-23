using UnityEngine;
using UnityEngine.UIElements;

namespace Atlas.Mvp.Unity
{
    public sealed partial class GameHud
    {
        void OnRootPointerDown(PointerDownEvent evt)
        {
            if (!_chatFocused) return;
            var ve = evt.target as VisualElement;
            while (ve != null)
            {
                if (ve == _chatInput) return;
                ve = ve.parent;
            }
            _chatInput.Blur();
        }

        void OnChatKeyDown(KeyDownEvent evt)
        {
            if (evt.keyCode == KeyCode.Escape)
            {
                _chatInput.value = string.Empty;
                _chatInput.Blur();
                evt.StopPropagation();
                return;
            }
            if (evt.keyCode != KeyCode.Return && evt.keyCode != KeyCode.KeypadEnter) return;
            string text = (_chatInput.value ?? string.Empty).Trim();
            _chatInput.value = string.Empty;
            evt.StopPropagation();
            if (text.Length == 0 || _owner == null || _owner.IsDestroyed) return;
            _owner.Cell.Say(text);
        }

        void OnChatBusReceived(uint senderId, string text)
        {
            var line = new Label($"#{senderId}: {text}");
            line.AddToClassList("chat-line");
            _chatScroll.contentContainer.Add(line);
            while (_chatScroll.contentContainer.childCount > kChatScrollback)
                _chatScroll.contentContainer.RemoveAt(0);
            _chatScroll.scrollOffset = new Vector2(0f, float.MaxValue);
        }
    }
}
