using System.Collections.Generic;

namespace Atlas.Mvp.Unity
{
    public interface ITickable     { void Tick(float dt); }
    public interface ILateTickable { void LateTick(); }

    // Central per-frame dispatcher. Bootstrap.Update/LateUpdate is the only
    // MonoBehaviour pump; everything else registers here so we pay one
    // native->managed call per frame instead of one per component.
    public static class Ticker
    {
        static readonly List<ITickable> _tick = new();
        static readonly List<ILateTickable> _late = new();
        static readonly List<object> _pendingAdd = new();
        static readonly List<object> _pendingRm = new();
        static bool _running;

        public static void Add(object o)
        {
            if (_running) { _pendingAdd.Add(o); return; }
            if (o is ITickable t) _tick.Add(t);
            if (o is ILateTickable l) _late.Add(l);
        }

        public static void Remove(object o)
        {
            if (_running) { _pendingRm.Add(o); return; }
            if (o is ITickable t) _tick.Remove(t);
            if (o is ILateTickable l) _late.Remove(l);
        }

        public static void RunTick(float dt)
        {
            _running = true;
            for (int i = 0; i < _tick.Count; ++i) _tick[i].Tick(dt);
            _running = false;
            Flush();
        }

        public static void RunLateTick()
        {
            _running = true;
            for (int i = 0; i < _late.Count; ++i) _late[i].LateTick();
            _running = false;
            Flush();
        }

        public static void Clear()
        {
            _tick.Clear();
            _late.Clear();
            _pendingAdd.Clear();
            _pendingRm.Clear();
        }

        static void Flush()
        {
            if (_pendingRm.Count > 0)
            {
                foreach (var o in _pendingRm)
                {
                    if (o is ITickable t) _tick.Remove(t);
                    if (o is ILateTickable l) _late.Remove(l);
                }
                _pendingRm.Clear();
            }
            if (_pendingAdd.Count > 0)
            {
                foreach (var o in _pendingAdd)
                {
                    if (o is ITickable t) _tick.Add(t);
                    if (o is ILateTickable l) _late.Add(l);
                }
                _pendingAdd.Clear();
            }
        }
    }
}
