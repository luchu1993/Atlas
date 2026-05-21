using System.Collections.Generic;
using Atlas.DataTypes;
using Atlas.Entity;
using Atlas.Serialization;

namespace Atlas.Mvp.Cell;

internal sealed class ProjectileSimulator
{
    private const float kGravity = 12f;
    private const float kLifetime = 2.0f;
    private const float kHitRadiusSq = 1.5f * 1.5f;
    private const int kDamage = 10;

    private struct Shot
    {
        public uint ShotId;
        public uint OwnerId;
        public Vector3 Position;
        public Vector3 Velocity;
        public double SpawnTime;
    }

    private static readonly Dictionary<uint, ProjectileSimulator> s_perSpace = new();

    public static ProjectileSimulator ForSpace(uint spaceId)
    {
        if (!s_perSpace.TryGetValue(spaceId, out var sim))
        {
            sim = new ProjectileSimulator();
            s_perSpace[spaceId] = sim;
        }
        return sim;
    }

    private readonly List<Shot> _active = new();
    private readonly Dictionary<uint, CellServerEntity> _targets = new();
    private uint _nextShotId = 1;
    private double _lastTickTime;

    public void RegisterTarget(CellServerEntity entity) => _targets[entity.EntityId] = entity;
    public void UnregisterTarget(uint entityId) => _targets.Remove(entityId);

    public uint Register(ServerEntity owner, Vector3 origin, Vector3 velocity)
    {
        var shot = new Shot
        {
            ShotId = _nextShotId++,
            OwnerId = owner.EntityId,
            Position = origin,
            Velocity = velocity,
            SpawnTime = Atlas.Time.ServerTime,
        };
        _active.Add(shot);
        return shot.ShotId;
    }

    public void Tick()
    {
        if (_active.Count == 0) return;
        var now = Atlas.Time.ServerTime;
        if (_lastTickTime <= 0) { _lastTickTime = now; return; }
        var dt = (float)(now - _lastTickTime);
        // Dedup multi-caller per tick; clamp resume jumps after long pauses.
        if (dt < 0.03f) return;
        if (dt > 0.2f) dt = 0.05f;
        _lastTickTime = now;

        for (int i = _active.Count - 1; i >= 0; --i)
        {
            var s = _active[i];
            s.Velocity = new Vector3(s.Velocity.X, s.Velocity.Y - kGravity * dt, s.Velocity.Z);
            s.Position = new Vector3(s.Position.X + s.Velocity.X * dt,
                                     s.Position.Y + s.Velocity.Y * dt,
                                     s.Position.Z + s.Velocity.Z * dt);

            var hit = TryHit(s);
            var grounded = s.Position.Y <= 0f;
            var expired = now - s.SpawnTime > kLifetime;

            if (hit != 0 || grounded || expired)
            {
                BroadcastEnd(s, hit);
                _active.RemoveAt(i);
            }
            else
            {
                _active[i] = s;
            }
        }
    }

    private uint TryHit(Shot s)
    {
        foreach (var kv in _targets)
        {
            if (kv.Key == s.OwnerId) continue;
            var entity = kv.Value;
            if (entity.IsDestroyed) continue;
            if (entity is not IDamageable dmg) continue;
            // Ghost Position is live (native read); Hp is not mirrored yet so
            // dead-skip only applies on the Real side.
            if (entity.IsReal && dmg.Hp <= 0) continue;
            var p = entity.Position;
            var dx = p.X - s.Position.X;
            // Target center sits ~1m above ground; projectile spawns at 1.2m.
            var dy = (p.Y + 1.0f) - s.Position.Y;
            var dz = p.Z - s.Position.Z;
            if (dx * dx + dy * dy + dz * dz > kHitRadiusSq) continue;

            if (entity.IsReal)
            {
                dmg.TakeDamage(kDamage, s.OwnerId);
            }
            else
            {
                RouteDamageToReal(entity, kDamage, s.OwnerId);
            }
            return entity.EntityId;
        }
        return 0;
    }

    private static void RouteDamageToReal(CellServerEntity target, int amount, uint attackerId)
    {
        int rpcId = target switch
        {
            Npc => Atlas.Rpc.RpcIds.Npc_TakeDamage,
            Avatar => Atlas.Rpc.RpcIds.Avatar_TakeDamage,
            _ => 0,
        };
        if (rpcId == 0) return;
        var writer = new SpanWriter(64);
        try
        {
            writer.WriteInt32(amount);
            writer.WriteUInt32(attackerId);
            target.InvokeCellMethodFromGhost(rpcId, writer.WrittenSpan);
        }
        finally { writer.Dispose(); }
    }

    private static void BroadcastEnd(Shot s, uint hitTarget)
    {
        var owner = EntityManager.Instance.Get(s.OwnerId);
        switch (owner)
        {
            case Avatar a: a.AllClients.OnProjectileEnded(s.ShotId, s.Position, hitTarget); break;
            case Npc n:    n.AllClients.OnProjectileEnded(s.ShotId, s.Position, hitTarget); break;
        }
    }
}
