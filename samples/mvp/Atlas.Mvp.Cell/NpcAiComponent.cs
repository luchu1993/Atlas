using System;
using Atlas.DataTypes;
using Atlas.Entity;
using Atlas.Entity.Components;
using Atlas.Mvp.Cell;

namespace Atlas.Components;

public sealed class NpcAiComponent : ServerLocalComponent
{
    private Npc Owner => (Npc)Entity;

    private const float kSpeed = 3.0f;
    private const float kReach = 0.5f;
    private const float kRetargetInterval = 4.0f;
    private const float kWanderRadius = 20.0f;
    private const float kWorldHalf = 100.0f;
    private const float kFireIntervalMin = 3.0f;
    private const float kFireIntervalMax = 7.0f;
    private const float kProjectileHorizSpeed = 12f;
    private const float kProjectileUpSpeed = 4f;

    private Vector3 _target;
    private float _retargetAccum;
    private float _fireAccum;
    private float _fireInterval;
    private Random _rng = null!;

    public override void OnAttached()
    {
        _rng = new Random(unchecked((int)(Entity.EntityId * 0x9E3779B1u)));
        PickTarget();
        RollFireInterval();
    }

    public override void OnTick(float dt)
    {
        Owner.Simulator.Tick();

        _retargetAccum += dt;
        _fireAccum += dt;
        var pos = Owner.Position;
        var dx = _target.X - pos.X;
        var dz = _target.Z - pos.Z;
        var distSq = dx * dx + dz * dz;
        if (_retargetAccum >= kRetargetInterval || distSq <= kReach * kReach)
        {
            _retargetAccum = 0f;
            PickTarget(pos);
            dx = _target.X - pos.X;
            dz = _target.Z - pos.Z;
            distSq = dx * dx + dz * dz;
            if (distSq <= kReach * kReach) return;
        }

        var dist = MathF.Sqrt(distSq);
        var step = MathF.Min(kSpeed * dt, dist);
        var inv = 1f / dist;
        Owner.Position = new Vector3(pos.X + dx * inv * step, pos.Y, pos.Z + dz * inv * step);
        Owner.Direction = new Vector3(dx * inv, 0f, dz * inv);

        if (_fireAccum >= _fireInterval)
        {
            _fireAccum = 0f;
            RollFireInterval();
            FireProjectile();
        }
    }

    private void RollFireInterval() =>
        _fireInterval = kFireIntervalMin +
                        (float)_rng.NextDouble() * (kFireIntervalMax - kFireIntervalMin);

    private void FireProjectile()
    {
        var dir = Owner.Direction;
        if (dir.X * dir.X + dir.Z * dir.Z < 0.01f) return;
        var pos = Owner.Position;
        var origin = new Vector3(pos.X + dir.X, pos.Y + 1.2f, pos.Z + dir.Z);
        var velocity = new Vector3(dir.X * kProjectileHorizSpeed,
                                   kProjectileUpSpeed,
                                   dir.Z * kProjectileHorizSpeed);
        var shotId = Owner.Simulator.Register(Owner, origin, velocity);
        Owner.AllClients.OnProjectileFired(shotId, origin, velocity);
    }

    private void PickTarget() => PickTarget(Owner.Position);

    private void PickTarget(Vector3 p)
    {
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            var tx = p.X + ((float)_rng.NextDouble() * 2f - 1f) * kWanderRadius;
            var tz = p.Z + ((float)_rng.NextDouble() * 2f - 1f) * kWanderRadius;
            tx = Math.Clamp(tx, -kWorldHalf, kWorldHalf);
            tz = Math.Clamp(tz, -kWorldHalf, kWorldHalf);
            var dxc = tx - p.X;
            var dzc = tz - p.Z;
            if (dxc * dxc + dzc * dzc >= 1f)
            {
                _target = new Vector3(tx, p.Y, tz);
                return;
            }
        }
        _target = new Vector3(-p.X * 0.5f, p.Y, -p.Z * 0.5f);
    }
}
