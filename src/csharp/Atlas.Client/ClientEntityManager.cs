using System;
using System.Collections.Generic;
using Atlas.DataTypes;
using Atlas.Diagnostics;
using Atlas.Serialization;

namespace Atlas.Client;

public sealed class ClientEntityManager
{
    private readonly Dictionary<uint, ClientEntity> _entities = new();
    private readonly ClientEntityFactoryRegistry _factory;
    private readonly ClientSession? _session;

    public event Action<ClientEntity>? EntityAdded;
    public event Action<ClientEntity>? EntityRemoved;

    public ClientEntityManager()
        : this(ClientEntityFactory.DefaultRegistry)
    {
    }

    internal ClientEntityManager(ClientEntityFactoryRegistry factory, ClientSession? session = null)
    {
        _factory = factory;
        _session = session;
    }

    public ClientEntity? Get(uint entityId)
    {
        _entities.TryGetValue(entityId, out var entity);
        return entity;
    }

    // Sticky + log-once: a corrupted entity stays skipped until a fresh AoI enter clears it.
    private static void MarkCorrupted(ClientEntity entity, string site, Exception ex)
    {
        if (entity.IsCorrupted) return;
        entity.IsCorrupted = true;
        Log.Error(
            $"ClientEntity[{entity.EntityId} type={entity.TypeName}] corrupted at {site}: {ex}. "
            + "Subsequent apply calls will be skipped until the entity re-enters AoI.");
    }

    public void Register(ClientEntity entity)
    {
        Register(entity, publish: true);
    }

    internal void Register(ClientEntity entity, bool publish)
    {
        if (_entities.TryGetValue(entity.EntityId, out var previous) &&
            !ReferenceEquals(previous, entity))
        {
            DestroyEntityInstance(previous);
            EntityRemoved?.Invoke(previous);
        }
        if (_session != null) entity.Session = _session;
        _entities[entity.EntityId] = entity;
        if (publish) EntityAdded?.Invoke(entity);
    }

    internal void PublishEntityAdded(ClientEntity entity)
    {
        if (!_entities.TryGetValue(entity.EntityId, out var current)) return;
        if (!ReferenceEquals(current, entity)) return;
        EntityAdded?.Invoke(entity);
    }

    public void Destroy(uint entityId)
    {
        if (_entities.TryGetValue(entityId, out var entity))
        {
            _entities.Remove(entityId);
            DestroyEntityInstance(entity);
            EntityRemoved?.Invoke(entity);
        }
    }

    // net_client fires no per-entity destroy callbacks during disconnect, so
    // hosts must drain the manager themselves on session teardown.
    public void Clear()
    {
        var removed = new List<ClientEntity>(_entities.Values);
        _entities.Clear();
        foreach (var entity in removed)
        {
            DestroyEntityInstance(entity);
            EntityRemoved?.Invoke(entity);
        }
    }

    public int Count => _entities.Count;

    public IEnumerable<ClientEntity> Entities => _entities.Values;

    // Drives AvatarFilter latency convergence. Hosts (Desktop loop / Unity Update)
    // call once per frame; peers with no samples short-circuit inside the filter.
    public void TickInterpolation(float dt)
    {
        foreach (var entity in _entities.Values)
        {
            if (entity.IsCorrupted) continue;
            entity.UpdateInterpolation(dt);
        }
    }

    // Idempotent re-enter: refreshes transform/state but does NOT re-fire OnInit / OnEnterWorld.
    public void OnEnter(uint entityId, ushort typeId, double serverTime,
                        Vector3 pos, Vector3 dir, bool onGround,
                        ReadOnlySpan<byte> peerSnapshot)
    {
        bool freshlyCreated = false;
        if (!_entities.TryGetValue(entityId, out var entity))
        {
            entity = _factory.Create(typeId);
            if (entity is null)
            {
                Log.Error(
                    $"ClientEntityManager.OnEnter: no factory registered for typeId={typeId} "
                    + $"(entityId={entityId})");
                return;
            }
            entity.EntityId = entityId;
            if (_session != null) entity.Session = _session;
            _entities[entityId] = entity;
            entity.OnInit();
            freshlyCreated = true;
        }

        entity.ApplyPositionUpdate(serverTime, pos, dir, onGround);

        if (!peerSnapshot.IsEmpty && !entity.IsCorrupted)
        {
            try
            {
                var reader = new SpanReader(peerSnapshot);
                entity.ApplyOtherSnapshot(ref reader);
            }
            catch (Exception ex) { MarkCorrupted(entity, "ApplyOtherSnapshot", ex); }
        }

        if (freshlyCreated)
        {
            entity.OnEnterWorld();
            EntityAdded?.Invoke(entity);
        }
    }

    public void OnLeave(uint entityId) => Destroy(entityId);

    public void ApplyPosition(uint entityId, double serverTime, Vector3 pos, Vector3 dir, bool onGround)
    {
        if (!_entities.TryGetValue(entityId, out var entity)) return;
        if (entity.IsCorrupted) return;
        try { entity.ApplyPositionUpdate(serverTime, pos, dir, onGround); }
        catch (Exception ex) { MarkCorrupted(entity, "ApplyPositionUpdate", ex); }
    }

    public void ApplyPropertyDelta(uint entityId, ulong eventSeq, ReadOnlySpan<byte> delta)
    {
        if (!_entities.TryGetValue(entityId, out var entity)) return;
        if (entity.IsCorrupted) return;
        entity.NoteIncomingEventSeq(eventSeq);
        if (delta.IsEmpty) return;
        try
        {
            var reader = new SpanReader(delta);
            entity.ApplyReplicatedDelta(ref reader);
        }
        catch (Exception ex) { MarkCorrupted(entity, "ApplyReplicatedDelta", ex); }
    }

    // Baseline is the recovery channel; clear IsCorrupted first, MarkCorrupted re-sets on throw.
    public void ApplyBaseline(uint entityId, ReadOnlySpan<byte> snapshot)
    {
        if (!_entities.TryGetValue(entityId, out var entity)) return;
        entity.IsCorrupted = false;
        if (snapshot.IsEmpty) return;
        try
        {
            var reader = new SpanReader(snapshot);
            entity.ApplyOwnerSnapshot(ref reader);
        }
        catch (Exception ex) { MarkCorrupted(entity, "ApplyOwnerSnapshot", ex); }
    }

    private static void DestroyEntityInstance(ClientEntity entity)
    {
        entity.IsDestroyed = true;
        entity.OnDestroy();
        entity.Session = null;
    }
}
