using System;
using System.Collections.Generic;
using Atlas.DataTypes;
using Atlas.Serialization;

namespace Atlas.Client;

public sealed class ClientEntityManager
{
    private readonly Dictionary<uint, ClientEntity> _entities = new();

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
        Console.Error.WriteLine(
            $"ClientEntity[{entity.EntityId} type={entity.TypeName}] corrupted at {site}: {ex}. "
            + "Subsequent apply calls will be skipped until the entity re-enters AoI.");
    }

    public void Register(ClientEntity entity)
    {
        _entities[entity.EntityId] = entity;
    }

    public void Destroy(uint entityId)
    {
        if (_entities.TryGetValue(entityId, out var entity))
        {
            entity.IsDestroyed = true;
            entity.OnDestroy();
            _entities.Remove(entityId);
        }
    }

    public int Count => _entities.Count;

    // Idempotent re-enter: refreshes transform/state but does NOT re-fire OnInit / OnEnterWorld.
    public void OnEnter(uint entityId, ushort typeId, Vector3 pos, Vector3 dir, bool onGround,
                        ReadOnlySpan<byte> peerSnapshot)
    {
        bool freshlyCreated = false;
        if (!_entities.TryGetValue(entityId, out var entity))
        {
            entity = ClientEntityFactory.Create(typeId);
            if (entity is null)
            {
                Console.Error.WriteLine(
                    $"ClientEntityManager.OnEnter: no factory registered for typeId={typeId} "
                    + $"(entityId={entityId})");
                return;
            }
            entity.EntityId = entityId;
            _entities[entityId] = entity;
            entity.OnInit();
            freshlyCreated = true;
        }

        entity.ApplyPositionUpdate(pos, dir, onGround);

        if (!peerSnapshot.IsEmpty && !entity.IsCorrupted)
        {
            try
            {
                var reader = new SpanReader(peerSnapshot);
                entity.ApplyOtherSnapshot(ref reader);
            }
            catch (Exception ex) { MarkCorrupted(entity, "ApplyOtherSnapshot", ex); }
        }

        if (freshlyCreated) entity.OnEnterWorld();
    }

    public void OnLeave(uint entityId) => Destroy(entityId);

    public void ApplyPosition(uint entityId, Vector3 pos, Vector3 dir, bool onGround)
    {
        if (!_entities.TryGetValue(entityId, out var entity)) return;
        if (entity.IsCorrupted) return;
        try { entity.ApplyPositionUpdate(pos, dir, onGround); }
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
}
