#if UNITY_2022_3_OR_NEWER

using System;
using System.Collections.Generic;
using System.Threading;
using Atlas.Coro;
using UnityEngine;
using UnityEngine.LowLevel;
using UnityEngine.PlayerLoop;

namespace Atlas.Client.Unity
{
    // IAtlasLoop on Unity's main thread; times by Time.unscaledTimeAsDouble.
    // Modelled on UniTask's PlayerLoopHelper.
    public sealed class UnityLoop : IAtlasLoop
    {
        private readonly Queue<PendingCallback> _nextFrame = new();
        private readonly List<TimerEntry> _timers = new();
        private long _nextTimerId;
        private long _currentFrame;
        private readonly int _mainThreadId;

        public UnityLoop()
        {
            _mainThreadId = Thread.CurrentThread.ManagedThreadId;
        }

        public bool IsMainThread => Thread.CurrentThread.ManagedThreadId == _mainThreadId;
        public long CurrentFrame => _currentFrame;

        public void PostNextFrame(Action<object?> cb, object? state)
        {
            if (cb is null) throw new ArgumentNullException(nameof(cb));
            _nextFrame.Enqueue(new PendingCallback(cb, state));
        }

        public void PostMainThread(Action<object?> cb, object? state)
        {
            if (cb is null) throw new ArgumentNullException(nameof(cb));
            // Job-system / async-op threads call this; PlayerLoop drains on main.
            lock (_nextFrame) _nextFrame.Enqueue(new PendingCallback(cb, state));
        }

        public long RegisterTimer(int milliseconds, Action<object?> cb, object? state)
        {
            if (cb is null) throw new ArgumentNullException(nameof(cb));
            if (milliseconds < 0) throw new ArgumentOutOfRangeException(nameof(milliseconds));
            var id = ++_nextTimerId;
            var dueTime = Time.unscaledTimeAsDouble + milliseconds / 1000.0;
            _timers.Add(new TimerEntry(id, dueTime, cb, state));
            return id;
        }

        public void CancelTimer(long handle)
        {
            for (var i = 0; i < _timers.Count; i++)
            {
                if (_timers[i].Id == handle) { _timers.RemoveAt(i); return; }
            }
        }

        // Called from the injected PlayerLoop slot once per render frame.
        public void Tick()
        {
            _currentFrame++;
            var now = Time.unscaledTimeAsDouble;

            for (var i = _timers.Count - 1; i >= 0; i--)
            {
                if (_timers[i].DueTime > now) continue;
                var t = _timers[i];
                _timers.RemoveAt(i);
                try { t.Callback(t.State); }
                catch (Exception ex) { Debug.LogException(ex); }
            }

            // Snapshot drain — re-enqueued callbacks land on next tick.
            int snapshot;
            lock (_nextFrame) snapshot = _nextFrame.Count;
            for (var i = 0; i < snapshot; i++)
            {
                PendingCallback item;
                lock (_nextFrame) item = _nextFrame.Dequeue();
                try { item.Callback(item.State); }
                catch (Exception ex) { Debug.LogException(ex); }
            }
        }

        private static UnityLoop? _instance;

        public static UnityLoop? Instance => _instance;

        // Idempotent: a second call replaces the previous instance.
        public static void Install()
        {
            Uninstall();
            var loop = new UnityLoop();
            _instance = loop;
            AtlasLoop.Install(loop);
            InjectPlayerLoopSystem(loop);
        }

        public static void Uninstall()
        {
            if (_instance is null) return;
            RemovePlayerLoopSystem();
            AtlasLoop.Reset();
            _instance = null;
        }

        private struct AtlasCoroPlayerLoopSystem { }

        private static void InjectPlayerLoopSystem(UnityLoop loop)
        {
            var current = PlayerLoop.GetCurrentPlayerLoop();
            current = InsertSubSystem<PreUpdate>(current,
                new PlayerLoopSystem
                {
                    type = typeof(AtlasCoroPlayerLoopSystem),
                    updateDelegate = loop.Tick,
                });
            PlayerLoop.SetPlayerLoop(current);
        }

        private static void RemovePlayerLoopSystem()
        {
            var current = PlayerLoop.GetCurrentPlayerLoop();
            current = RemoveSubSystemByType(current, typeof(AtlasCoroPlayerLoopSystem));
            PlayerLoop.SetPlayerLoop(current);
        }

        private static PlayerLoopSystem InsertSubSystem<TParent>(PlayerLoopSystem root,
            PlayerLoopSystem newSub)
        {
            var subs = root.subSystemList ?? Array.Empty<PlayerLoopSystem>();
            var rebuilt = new PlayerLoopSystem[subs.Length];
            for (var i = 0; i < subs.Length; i++)
            {
                rebuilt[i] = subs[i];
                if (subs[i].type == typeof(TParent))
                {
                    var inner = subs[i].subSystemList ?? Array.Empty<PlayerLoopSystem>();
                    var withOurs = new PlayerLoopSystem[inner.Length + 1];
                    withOurs[0] = newSub;       // run before Unity's PreUpdate body
                    Array.Copy(inner, 0, withOurs, 1, inner.Length);
                    rebuilt[i].subSystemList = withOurs;
                }
            }
            root.subSystemList = rebuilt;
            return root;
        }

        private static PlayerLoopSystem RemoveSubSystemByType(PlayerLoopSystem root, Type marker)
        {
            var subs = root.subSystemList;
            if (subs is null) return root;
            for (var i = 0; i < subs.Length; i++)
            {
                var inner = subs[i].subSystemList;
                if (inner is null) continue;
                var idx = -1;
                for (var j = 0; j < inner.Length; j++)
                {
                    if (inner[j].type == marker) { idx = j; break; }
                }
                if (idx < 0) continue;
                var trimmed = new PlayerLoopSystem[inner.Length - 1];
                Array.Copy(inner, 0, trimmed, 0, idx);
                Array.Copy(inner, idx + 1, trimmed, idx, inner.Length - idx - 1);
                subs[i].subSystemList = trimmed;
            }
            return root;
        }

        private readonly struct PendingCallback
        {
            public readonly Action<object?> Callback;
            public readonly object? State;
            public PendingCallback(Action<object?> cb, object? s) { Callback = cb; State = s; }
        }

        private struct TimerEntry
        {
            public long Id;
            public double DueTime;
            public Action<object?> Callback;
            public object? State;
            public TimerEntry(long id, double dueTime, Action<object?> cb, object? s)
            { Id = id; DueTime = dueTime; Callback = cb; State = s; }
        }
    }

    public static class UnityLoopBootstrap
    {
        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.SubsystemRegistration)]
        private static void AutoInstall()
        {
            UnityLoop.Install();
        }
    }
}

#endif
