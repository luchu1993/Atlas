using System;
using System.Collections.Generic;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;
using MvpAvatar = Atlas.Mvp.Client.Avatar;
using MvpNpc = Atlas.Mvp.Client.Npc;

namespace Atlas.Mvp.Unity
{
    public sealed class ViewRegistry : IDisposable
    {
        readonly AtlasNetworkManager _net;
        readonly Transform _worldRoot;
        readonly Action<AvatarView, MvpAvatar> _onOwnerAttached;
        readonly Dictionary<uint, EntityView> _views = new();
        readonly Dictionary<Type, Func<ClientEntity, EntityView>> _factories = new();
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
            Register<MvpAvatar>(avatar => new AvatarView(avatar, _net, _worldRoot));
            Register<MvpNpc>(npc => new NpcView(npc, _net, _worldRoot));
            _net.Session.EntityManager.EntityAdded += OnEntityAdded;
            _net.Session.EntityManager.EntityRemoved += OnEntityRemoved;
            foreach (var entity in _net.Session.EntityManager.Entities)
                OnEntityAdded(entity);
        }

        public void Dispose()
        {
            _net.Session.EntityManager.EntityAdded -= OnEntityAdded;
            _net.Session.EntityManager.EntityRemoved -= OnEntityRemoved;
            foreach (var v in _views.Values) v?.Dispose();
            _views.Clear();
            _ownerEntityId = 0;
        }

        public void Register<T>(Func<T, EntityView> factory) where T : ClientEntity
        {
            _factories[typeof(T)] = entity => factory((T)entity);
        }

        void OnEntityAdded(ClientEntity entity)
        {
            if (_views.ContainsKey(entity.EntityId)) return;
            if (!_factories.TryGetValue(entity.GetType(), out var factory)) return;
            var view = factory(entity);
            _views[entity.EntityId] = view;
            ++_aoiEnterCount;
            if (entity.IsOwner && entity is MvpAvatar avatar)
            {
                _ownerEntityId = entity.EntityId;
                _onOwnerAttached((AvatarView)view, avatar);
            }
        }

        void OnEntityRemoved(ClientEntity entity)
        {
            if (!_views.TryGetValue(entity.EntityId, out var view)) return;
            view.Dispose();
            _views.Remove(entity.EntityId);
            ++_aoiLeaveCount;
        }
    }
}
