using System;
using System.Collections.Generic;
using Atlas.DataTypes;
using Atlas.Serialization;

namespace Atlas.Client;

/// <summary>
/// Client-side entity lifecycle and state-update dispatcher.
///
/// The four <c>OnXxx</c> / <c>ApplyXxx</c> entry points are driven by the
/// native transport layer (see <see cref="ClientCallbacks.DeliverFromServer"/>)
/// and correspond to the four <c>CellAoIEnvelope</c> kinds plus the baseline
/// snapshot channel. Envelope decoding (kind byte, entity id) happens in
/// <see cref="ClientCallbacks"/>; this manager only sees already-parsed
/// primitives and the inner payload bytes.
/// </summary>
public sealed class ClientEntityManager
{
    private readonly Dictionary<uint, ClientEntity> _entities = new();

    public ClientEntity? Get(uint entityId)
    {
        _entities.TryGetValue(entityId, out var entity);
        return entity;
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

    /// <summary>
    /// AoI peer entered the local player's view. Creates the entity via the
    /// generated factory, initialises identity + transform, and applies the
    /// other-scope <paramref name="peerSnapshot"/> via
    /// <see cref="ClientEntity.ApplyOtherSnapshot"/> (paired with the
    /// server's <c>SerializeForOtherClients</c>). The snapshot write goes
    /// straight to backing fields and does not fire property-change
    /// callbacks — the peer is entering in a given state rather than
    /// transitioning.
    /// After all initial state is in place, fires
    /// <see cref="ClientEntity.OnEnterWorld"/> so scripts can do setup
    /// that needs the complete starting state.
    /// Idempotent: a re-enter of an already-tracked entity refreshes
    /// transform + state but does NOT re-fire
    /// <see cref="ClientEntity.OnInit"/> or
    /// <see cref="ClientEntity.OnEnterWorld"/> — those are once-per-lifetime
    /// hooks.
    /// </summary>
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

        if (!peerSnapshot.IsEmpty)
        {
            var reader = new SpanReader(peerSnapshot);
            entity.ApplyOtherSnapshot(ref reader);
        }

        if (freshlyCreated) entity.OnEnterWorld();
    }

    /// <summary>
    /// AoI peer left the local player's view. Tears down the client-side
    /// entity without touching the server — the entity still exists
    /// authoritatively on CellApp, it is only no longer visible here.
    /// </summary>
    public void OnLeave(uint entityId) => Destroy(entityId);

    /// <summary>
    /// Apply a volatile position/direction update. The inner payload layout
    /// is <c>pos(3f) + dir(3f) + on_ground(u8)</c> (see
    /// <c>witness.cc::SendEntityUpdate</c> volatile branch).
    /// </summary>
    public void ApplyPosition(uint entityId, Vector3 pos, Vector3 dir, bool onGround)
    {
        if (_entities.TryGetValue(entityId, out var entity))
            entity.ApplyPositionUpdate(pos, dir, onGround);
    }

    /// <summary>
    /// Apply an ordered property delta (<c>kEntityPropertyUpdate</c>). The
    /// caller supplies the <paramref name="eventSeq"/> taken from the
    /// envelope prefix: the entity records it, logs a gap warning if the
    /// seq jumped by more than 1, then dispatches the delta bytes through
    /// the generator-emitted
    /// <see cref="ClientEntity.ApplyReplicatedDelta"/>.
    /// </summary>
    public void ApplyPropertyDelta(uint entityId, ulong eventSeq, ReadOnlySpan<byte> delta)
    {
        if (!_entities.TryGetValue(entityId, out var entity)) return;
        entity.NoteIncomingEventSeq(eventSeq);
        if (delta.IsEmpty) return;
        var reader = new SpanReader(delta);
        entity.ApplyReplicatedDelta(ref reader);
    }

    /// <summary>
    /// Apply a periodic full-state baseline snapshot (channel
    /// <c>kClientBaselineMessageId = 0xF002</c>). The baseline body is the
    /// owner-scope snapshot produced by <c>SerializeForOwnerClient</c>;
    /// routes through <see cref="ClientEntity.ApplyOwnerSnapshot"/> so
    /// fields are reset without firing change callbacks.
    /// </summary>
    public void ApplyBaseline(uint entityId, ReadOnlySpan<byte> snapshot)
    {
        if (!_entities.TryGetValue(entityId, out var entity)) return;
        if (snapshot.IsEmpty) return;
        var reader = new SpanReader(snapshot);
        entity.ApplyOwnerSnapshot(ref reader);
    }
}
