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
    /// generated factory and initialises identity + transform. The envelope
    /// also carries a scope-subset <paramref name="peerSnapshot"/> produced by
    /// the server's <c>SerializeForOtherClients</c>; however the current
    /// <see cref="ClientEntity.Deserialize"/> expects a
    /// <c>version+fieldCount+bodyLength</c>-framed payload, which the scope
    /// serializers do NOT emit. Applying the snapshot is therefore gated
    /// behind Phase B0 of the client-sync rework; until B0 lands we log the
    /// snapshot byte count and drop the bytes.
    /// Idempotent: a re-enter after a pending leave reuses the existing
    /// instance and only refreshes the transform.
    /// </summary>
    public void OnEnter(uint entityId, ushort typeId, Vector3 pos, Vector3 dir, bool onGround,
                        ReadOnlySpan<byte> peerSnapshot)
    {
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
        }

        entity.ApplyPositionUpdate(pos, dir, onGround);

        if (!peerSnapshot.IsEmpty)
        {
            // TODO(B0): scope-subset snapshot has no version/fieldCount/bodyLength
            // framing, so calling entity.Deserialize here would misread the bytes.
            // Drop for now — B0 introduces a paired scope-aware deserializer.
            Console.Error.WriteLine(
                $"ClientEntityManager.OnEnter: dropping {peerSnapshot.Length}-byte snapshot for "
                + $"entityId={entityId} (waiting on Phase B0 scope-aware deserializer)");
        }
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
    /// Apply an ordered property delta (<c>kEntityPropertyUpdate</c>).
    /// The base <see cref="ClientEntity.ApplyReplicatedDelta"/> is a no-op
    /// until Phase B1 lands a generated override; this method is still
    /// wired so the dispatcher can be smoke-tested end-to-end today.
    /// </summary>
    public void ApplyPropertyDelta(uint entityId, ReadOnlySpan<byte> delta)
    {
        if (!_entities.TryGetValue(entityId, out var entity)) return;
        if (delta.IsEmpty) return;
        var reader = new SpanReader(delta);
        entity.ApplyReplicatedDelta(ref reader);
    }

    /// <summary>
    /// Apply a periodic full-state baseline snapshot (channel
    /// <c>kClientBaselineMessageId = 0xF002</c>). The baseline body is the
    /// owner-scope snapshot produced by <c>SerializeForOwnerClient</c>,
    /// which — like the AoI enter snapshot — lacks the framing that
    /// <see cref="ClientEntity.Deserialize"/> requires. Phase B0 will
    /// introduce a scope-aware deserializer; until then we drop the payload
    /// and log a breadcrumb so the test harness can see baselines arriving.
    /// </summary>
    public void ApplyBaseline(uint entityId, ReadOnlySpan<byte> snapshot)
    {
        if (!_entities.TryGetValue(entityId, out _)) return;
        if (snapshot.IsEmpty) return;
        // TODO(B0): gated on scope-aware deserializer — see OnEnter.
        Console.Error.WriteLine(
            $"ClientEntityManager.ApplyBaseline: dropping {snapshot.Length}-byte baseline for "
            + $"entityId={entityId} (waiting on Phase B0 scope-aware deserializer)");
    }
}
