using System;
using System.Collections.Generic;

namespace Atlas.Entity;

/// <summary>
/// Central registry for all <see cref="ServerEntity"/> instances.
/// Handles lifecycle dispatch (Init/Tick/Shutdown) and deferred create/destroy.
/// </summary>
public sealed class EntityManager
{
    public static EntityManager Instance { get; } = new();
    private EntityManager() { }

    private readonly Dictionary<uint, ServerEntity> _entities = new();

    // Distributed EntityId: high 8 bits = process prefix, low 24 bits = local seq.
    // Process prefix is provided by C++ via atlas_get_process_prefix().
    private byte _processPrefix;
    private uint _localSeq = 1;

    private readonly List<ServerEntity> _pendingCreates = new();
    private readonly List<uint> _pendingDestroys = new();
    private bool _iterating;

    internal void SetProcessPrefix(byte prefix) => _processPrefix = prefix;

    /// <summary>Create a new entity. If called during iteration, creation is deferred.</summary>
    public T Create<T>() where T : ServerEntity, new()
    {
        var entity = new T { EntityId = AllocateEntityId() };
        if (_iterating)
            _pendingCreates.Add(entity);
        else
            _entities[entity.EntityId] = entity;
        return entity;
    }

    /// <summary>Destroy an entity. If called during iteration, destruction is deferred.</summary>
    public void Destroy(uint entityId)
    {
        if (_iterating)
        {
            _pendingDestroys.Add(entityId);
            return;
        }
        DestroyImmediate(entityId);
    }

    public ServerEntity? Get(uint entityId)
    {
        _entities.TryGetValue(entityId, out var entity);
        return entity;
    }

    public int Count => _entities.Count;

    internal void OnInitAll(bool isReload)
    {
        _iterating = true;
        foreach (var entity in _entities.Values)
            entity.OnInit(isReload);
        FlushPending();
    }

    internal void OnTickAll(float deltaTime)
    {
        _iterating = true;
        foreach (var entity in _entities.Values)
        {
            if (!entity.IsDestroyed)
                entity.OnTick(deltaTime);
        }
        FlushPending();
    }

    internal void OnShutdownAll()
    {
        foreach (var entity in _entities.Values)
            entity.OnDestroy();
        _entities.Clear();
    }

    /// <summary>Get all entities (used by hot-reload serialization).</summary>
    internal IReadOnlyCollection<ServerEntity> GetAllEntities() => _entities.Values;

    /// <summary>Register an existing entity (used by hot-reload deserialization).</summary>
    internal void Register(ServerEntity entity) => _entities[entity.EntityId] = entity;

    /// <summary>Clear all entities without calling OnDestroy (used before hot-reload unload).</summary>
    internal void Clear() => _entities.Clear();

    private uint AllocateEntityId()
    {
        if (_localSeq >= 0x0100_0000)
            throw new InvalidOperationException("EntityId local sequence overflow");
        return ((uint)_processPrefix << 24) | _localSeq++;
    }

    private void DestroyImmediate(uint entityId)
    {
        if (_entities.Remove(entityId, out var entity))
        {
            entity.OnDestroy();
            entity.IsDestroyed = true;
        }
    }

    private void FlushPending()
    {
        _iterating = false;
        foreach (var entity in _pendingCreates)
            _entities[entity.EntityId] = entity;
        _pendingCreates.Clear();
        foreach (var id in _pendingDestroys)
            DestroyImmediate(id);
        _pendingDestroys.Clear();
    }
}
