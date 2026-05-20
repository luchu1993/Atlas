using System;
using System.Collections.Generic;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;
using MvpNpc = Atlas.Mvp.Client.Npc;

namespace Atlas.Mvp.Unity
{
    public sealed class ViewRegistry : ITickable, IDisposable
    {
        readonly AtlasNetworkManager _net;
        readonly Transform _worldRoot;
        readonly Action<AvatarView, MvpAvatar> _onOwnerAttached;
        readonly Dictionary<uint, EntityView> _views = new();
        readonly List<uint> _stale = new();
        uint _ownerEntityId;
        uint _aoiEnterCount;
        uint _aoiLeaveCount;

        public uint OwnerEntityId => _ownerEntityId;
        public uint AoiEnterCount => _aoiEnterCount;
        public uint AoiLeaveCount => _aoiLeaveCount;

        // NPCs currently inside the owner's AoI envelope (= materialised
        // local views). Cheap O(N) scan; views count is bounded by AoI.
        public int NpcViewCount
        {
            get
            {
                int n = 0;
                foreach (var v in _views.Values)
                    if (v?.Entity is MvpNpc) ++n;
                return n;
            }
        }

        public ViewRegistry(AtlasNetworkManager net, Transform worldRoot,
            Action<AvatarView, MvpAvatar> onOwnerAttached)
        {
            _net = net;
            _worldRoot = worldRoot;
            _onOwnerAttached = onOwnerAttached;
            Ticker.Add(this);
        }

        public void Dispose()
        {
            Ticker.Remove(this);
            foreach (var v in _views.Values) v?.Dispose();
            _views.Clear();
            _ownerEntityId = 0;
        }

        public void Tick(float dt)
        {
            foreach (var entity in ClientCallbacks.EntityManager.Entities)
            {
                if (_views.ContainsKey(entity.EntityId)) continue;
                if (entity is MvpAvatar || entity is MvpNpc)
                {
                    _views[entity.EntityId] = Spawn(entity);
                    ++_aoiEnterCount;
                }
            }
            _stale.Clear();
            foreach (var kv in _views)
            {
                if (kv.Value == null || kv.Value.Entity == null || kv.Value.Entity.IsDestroyed)
                    _stale.Add(kv.Key);
            }
            foreach (var id in _stale)
            {
                _views[id]?.Dispose();
                _views.Remove(id);
                ++_aoiLeaveCount;
            }
        }

        EntityView Spawn(ClientEntity entity)
        {
            // Reconcile already filters to Avatar / Npc; the default arm
            // exists only because switch expressions must be exhaustive.
            EntityView view = entity switch
            {
                MvpAvatar a => new AvatarView(a, _net, _worldRoot),
                MvpNpc n => new NpcView(n, _net, _worldRoot),
                _ => throw new InvalidOperationException(
                         $"Spawn: unsupported entity type {entity.TypeName}"),
            };
            if (entity.IsOwner && entity is MvpAvatar avatar)
            {
                // Owner may switch via EntityTransferred (Account → Avatar handoff).
                _ownerEntityId = entity.EntityId;
                _onOwnerAttached((AvatarView)view, avatar);
            }
            return view;
        }
    }
}
