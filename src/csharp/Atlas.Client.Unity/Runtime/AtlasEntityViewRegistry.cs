using System;
using System.Collections.Generic;
using Atlas.Client;

namespace Atlas.Client.Unity
{
    public interface IAtlasEntityView : IDisposable
    {
        ClientEntity Entity { get; }
    }

    public class AtlasEntityViewRegistry<TView> : IDisposable
        where TView : class, IAtlasEntityView
    {
        readonly ClientSession _session;
        readonly Dictionary<uint, TView> _views = new();
        readonly Dictionary<Type, Func<ClientEntity, TView>> _factories = new();
        bool _tracking;
        bool _disposed;

        public AtlasEntityViewRegistry(ClientSession session)
        {
            _session = session ?? throw new ArgumentNullException(nameof(session));
        }

        public int Count => _views.Count;
        protected IEnumerable<TView> Views => _views.Values;

        public void Register<TEntity>(Func<TEntity, TView> factory)
            where TEntity : ClientEntity
        {
            if (factory == null) throw new ArgumentNullException(nameof(factory));
            _factories[typeof(TEntity)] = entity => factory((TEntity)entity);
        }

        public bool TryGetView(uint entityId, out TView view) =>
            _views.TryGetValue(entityId, out view!);

        public void StartTracking()
        {
            if (_tracking) return;
            _tracking = true;
            _session.EntityManager.EntityAdded += OnEntityAdded;
            _session.EntityManager.EntityRemoved += OnEntityRemoved;
            foreach (var entity in _session.EntityManager.Entities)
                OnEntityAdded(entity);
        }

        public virtual void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            if (_tracking)
            {
                _session.EntityManager.EntityAdded -= OnEntityAdded;
                _session.EntityManager.EntityRemoved -= OnEntityRemoved;
                _tracking = false;
            }
            foreach (var view in _views.Values)
                view.Dispose();
            _views.Clear();
        }

        protected virtual void OnViewAdded(TView view, ClientEntity entity) { }
        protected virtual void OnViewRemoved(TView view, ClientEntity entity) { }

        void OnEntityAdded(ClientEntity entity)
        {
            if (_views.ContainsKey(entity.EntityId)) return;
            if (!_factories.TryGetValue(entity.GetType(), out var factory)) return;
            var view = factory(entity);
            _views[entity.EntityId] = view;
            OnViewAdded(view, entity);
        }

        void OnEntityRemoved(ClientEntity entity)
        {
            if (!_views.TryGetValue(entity.EntityId, out var view)) return;
            _views.Remove(entity.EntityId);
            OnViewRemoved(view, entity);
            view.Dispose();
        }
    }
}
