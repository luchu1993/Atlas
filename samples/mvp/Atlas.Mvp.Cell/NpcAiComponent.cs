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

    // Target / retarget timer / fire interval live on Npc as cell_private
    // properties so cross-cellapp offload preserves them via persistent_blob.
    // _fireAccum stays local — drift on offload is at most one fire cycle.
    private float _fireAccum;
    private Random _rng = null!;

    public override void OnAttached()
    {
        // RNG state is intentionally not carried across offload; reseed
        // each attach. Initial AiTarget / AiFireInterval are seeded by
        // Npc.OnInit on fresh spawn; offload-arrived NPCs already have
        // both values restored from persistent_blob.
        _rng = new Random();
    }

    // Invoked by Npc.OnInit on fresh spawn only. Direction-biased target
    // keeps boundary-spawned NPCs walking forward instead of bouncing
    // back across the BSP split.
    internal void InitFreshState()
    {
        var dir = Owner.Direction;
        if (dir.X * dir.X + dir.Z * dir.Z > 0.25f)
        {
            var p = Owner.Position;
            float tx = Math.Clamp(p.X + dir.X * kWanderRadius, -kWorldHalf, kWorldHalf);
            float tz = Math.Clamp(p.Z + dir.Z * kWanderRadius, -kWorldHalf, kWorldHalf);
            Owner.AiTarget = new Vector3(tx, p.Y, tz);
        }
        else
        {
            PickTarget();
        }
        RollFireInterval();
    }

    public override void OnTick(float dt)
    {
        // Two re-checks: BroadcastDamage may DestroySelf inside Simulator.Tick
        // (peer projectile hit), and the AI tick may run one frame past detach.
        if (Owner.IsDestroyed) return;
        Owner.Simulator.Tick();
        if (Owner.IsDestroyed) return;

        Owner.AiRetargetAccum += dt;
        _fireAccum += dt;
        var pos = Owner.Position;
        var target = Owner.AiTarget;
        var dx = target.X - pos.X;
        var dz = target.Z - pos.Z;
        var distSq = dx * dx + dz * dz;
        if (Owner.AiRetargetAccum >= kRetargetInterval || distSq <= kReach * kReach)
        {
            Owner.AiRetargetAccum = 0f;
            PickTarget(pos);
            target = Owner.AiTarget;
            dx = target.X - pos.X;
            dz = target.Z - pos.Z;
            distSq = dx * dx + dz * dz;
            if (distSq <= kReach * kReach) return;
        }

        var dist = MathF.Sqrt(distSq);
        var step = MathF.Min(kSpeed * dt, dist);
        var inv = 1f / dist;
        Owner.Position = new Vector3(pos.X + dx * inv * step, pos.Y, pos.Z + dz * inv * step);
        Owner.Direction = new Vector3(dx * inv, 0f, dz * inv);

        if (_fireAccum >= Owner.AiFireInterval)
        {
            _fireAccum = 0f;
            RollFireInterval();
            FireProjectile();
        }
    }

    private void RollFireInterval() =>
        Owner.AiFireInterval = kFireIntervalMin +
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
                Owner.AiTarget = new Vector3(tx, p.Y, tz);
                return;
            }
        }
        Owner.AiTarget = new Vector3(-p.X * 0.5f, p.Y, -p.Z * 0.5f);
    }
}
