using System.Collections.Generic;

namespace Atlas.Client.Unity
{
    public interface IAtlasUnityTickable { void Tick(float dt); }
    public interface IAtlasUnityLateTickable { void LateTick(); }

    public sealed class AtlasUnityFramePump
    {
        readonly List<IAtlasUnityTickable> _tick = new();
        readonly List<IAtlasUnityLateTickable> _late = new();
        readonly List<object> _pendingAdd = new();
        readonly List<object> _pendingRemove = new();
        bool _running;

        public void Add(object o)
        {
            if (_running) { _pendingAdd.Add(o); return; }
            if (o is IAtlasUnityTickable t && !_tick.Contains(t)) _tick.Add(t);
            if (o is IAtlasUnityLateTickable l && !_late.Contains(l)) _late.Add(l);
        }

        public void Remove(object o)
        {
            if (_running) { _pendingRemove.Add(o); return; }
            if (o is IAtlasUnityTickable t) _tick.Remove(t);
            if (o is IAtlasUnityLateTickable l) _late.Remove(l);
        }

        public void RunTick(float dt)
        {
            _running = true;
            for (int i = 0; i < _tick.Count; ++i) _tick[i].Tick(dt);
            _running = false;
            Flush();
        }

        public void RunLateTick()
        {
            _running = true;
            for (int i = 0; i < _late.Count; ++i) _late[i].LateTick();
            _running = false;
            Flush();
        }

        public void Clear()
        {
            _tick.Clear();
            _late.Clear();
            _pendingAdd.Clear();
            _pendingRemove.Clear();
            _running = false;
        }

        void Flush()
        {
            foreach (var o in _pendingRemove) Remove(o);
            _pendingRemove.Clear();
            foreach (var o in _pendingAdd) Add(o);
            _pendingAdd.Clear();
        }
    }
}
