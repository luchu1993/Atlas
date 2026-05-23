using UnityEngine;
using UnityEngine.UIElements;

namespace Atlas.Mvp.Unity
{
    public sealed partial class GameHud
    {
        void OnLookDown(PointerDownEvent evt)
        {
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
    }
}
