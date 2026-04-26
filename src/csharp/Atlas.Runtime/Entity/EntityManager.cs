using System;
using System.Collections.Generic;
using Atlas.Core;
using Atlas.Diagnostics;
using Atlas.Serialization;

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
    // Process prefix is provided by C++ via AtlasGetProcessPrefix().
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
            if (entity.IsDestroyed) continue;
            // entity.GetType().Name is a CLR-interned string — no allocation per call.
            using (Profiler.ZoneN(entity.GetType().Name))
                entity.OnTick(deltaTime);
            // Fire component ticks AFTER the entity body so a component
            // that depends on entity state sees the latest values; the
            // pump still captures any dirty bits both touch this tick.
            using (Profiler.ZoneN(ProfilerNames.ScriptComponentTickAll))
                entity.TickAllComponents(deltaTime);
        }
        FlushPending();
    }

    /// <summary>
    /// Drain each entity's per-tick replication output and hand it to the
    /// cell layer via <see cref="NativeApi.PublishReplicationFrame"/>. Called
    /// by <see cref="Atlas.Core.Lifecycle.DoOnTick"/> immediately after
    /// <see cref="OnTickAll"/> so the pump captures any property / volatile
    /// state that user code flipped during OnTick.
    /// <para/>
    /// Without this, the generator-emitted
    /// <see cref="ServerEntity.BuildAndConsumeReplicationFrame"/> overrides
    /// would never run — event_seq and volatile_seq on the C++ side would
    /// stay at 0, Witness::Update would conclude "peer already up to date"
    /// and no property deltas would ever reach any client.
    /// <para/>
    /// Runs on every process type; on BaseApp / Client
    /// <see cref="NativeApi.PublishReplicationFrame"/> is a provider-side
    /// no-op (with a one-time warning), so a shared-script entity's pump
    /// cost is one virtual call plus one P/Invoke per tick.
    /// </summary>
    internal void PublishReplicationAll()
    {
        // Single shared quartet of writers — Reset between entities — so we
        // do one ArrayPool.Rent per Reset cap, not one per entity per tick.
        // See ServerEntity.BuildAndConsumeReplicationFrame docstring "ZERO-
        // ALLOC CONTRACT".
        var ownerSnap = new SpanWriter(256);
        var otherSnap = new SpanWriter(256);
        var ownerDelta = new SpanWriter(128);
        var otherDelta = new SpanWriter(128);
        try
        {
            _iterating = true;
            foreach (var entity in _entities.Values)
            {
                if (entity.IsDestroyed) continue;

                ownerSnap.Reset();
                otherSnap.Reset();
                ownerDelta.Reset();
                otherDelta.Reset();

                if (!entity.BuildAndConsumeReplicationFrame(
                        ref ownerSnap, ref otherSnap,
                        ref ownerDelta, ref otherDelta,
                        out var eventSeq, out var volatileSeq))
                {
                    continue;  // fast path — nothing changed this tick
                }

                NativeApi.PublishReplicationFrame(entity.EntityId, eventSeq, volatileSeq,
                    ownerSnap.WrittenSpan, otherSnap.WrittenSpan,
                    ownerDelta.WrittenSpan, otherDelta.WrittenSpan);
            }
            FlushPending();
        }
        finally
        {
            ownerSnap.Dispose();
            otherSnap.Dispose();
            ownerDelta.Dispose();
            otherDelta.Dispose();
        }
    }

    internal void OnShutdownAll()
    {
        _iterating = true;
        foreach (var entity in _entities.Values)
            entity.OnDestroy();
        _iterating = false;
        _pendingCreates.Clear();
        _pendingDestroys.Clear();
        _entities.Clear();
    }

    /// <summary>
    /// Full reset: clear entities, reset ID sequence and process prefix.
    /// Called by <see cref="Atlas.Core.EngineContext.Shutdown"/> to ensure
    /// a clean slate for re-initialization within the same process.
    /// </summary>
    internal void Reset()
    {
        _entities.Clear();
        _pendingCreates.Clear();
        _pendingDestroys.Clear();
        _localSeq = 1;
        _processPrefix = 0;
        _iterating = false;
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
