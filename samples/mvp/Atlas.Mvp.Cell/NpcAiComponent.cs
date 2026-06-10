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
    // Reaps nav walks the controller could not finish (blocked goal);
    // the longest legitimate detour is ~40 m at 3 m/s ≈ 13 s.
    private const float kNavRetargetTimeout = 15.0f;
    private const float kWanderRadius = 20.0f;
    private const float kWorldHalf = 100.0f;
    private const float kFireIntervalMin = 3.0f;
    private const float kFireIntervalMax = 7.0f;
    private const float kProjectileHorizSpeed = 12f;
    private const float kProjectileUpSpeed = 4f;

    // Wander state lives on Npc as cell_private properties so offload preserves
    // it; these locals re-derive (the walk controller itself migrates natively).
    private float _fireAccum;
    private bool _navWalking;
    private bool _navFailedThisLeg;
    private Random _rng = null!;

    public override void OnAttached()
    {
        // RNG reseeds on every attach; AiTarget / AiFireInterval come from
        // Npc.OnInit on fresh spawn or from persistent_blob after offload.
        _rng = new Random();
    }

    // Fresh-spawn only: a direction-biased first target keeps boundary-spawned
    // NPCs walking forward instead of bouncing back across the BSP split.
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
        var arrived = distSq <= kReach * kReach;

        if (_navWalking && !arrived && Owner.AiRetargetAccum < kNavRetargetTimeout)
        {
            TickFire(dx, dz, distSq);
            return;  // the migrating MoveAlongPath controller owns the walk
        }

        // A nav leg only reaches here ended (arrived or timed out), so it always
        // retargets; intent mode keeps the original cadence.
        var retargetDue = _navWalking || arrived || Owner.AiRetargetAccum >= kRetargetInterval;
        _navWalking = false;
        if (retargetDue)
        {
            Owner.AiRetargetAccum = 0f;
            _navFailedThisLeg = false;
            PickTarget(pos);
            target = Owner.AiTarget;
            dx = target.X - pos.X;
            dz = target.Z - pos.Z;
            distSq = dx * dx + dz * dz;
            if (distSq <= kReach * kReach)
            {
                Owner.SetMovementIntent(Vector3.Zero, 0f);
                return;
            }
        }

        if (!_navFailedThisLeg)
        {
            if (Owner.NavMoveTo(target, kSpeed) != 0)
            {
                _navWalking = true;
                Owner.SetMovementIntent(Vector3.Zero, 0f);  // hand the walk to the controller
                TickFire(dx, dz, distSq);
                return;
            }
            _navFailedThisLeg = true;  // one failed plan per leg, not one per tick
        }

        // No navmesh (or off-mesh target): straight-line wander as before.
        var dist = MathF.Sqrt(distSq);
        var inv = 1f / dist;
        var dir = new Vector3(dx * inv, 0f, dz * inv);
        Owner.SetMovementIntent(dir, kSpeed);
        TickFire(dx, dz, distSq);
    }

    private void TickFire(float dx, float dz, float distSq)
    {
        if (_fireAccum < Owner.AiFireInterval) return;
        _fireAccum = 0f;
        RollFireInterval();
        if (distSq < 1e-4f) return;
        var inv = 1f / MathF.Sqrt(distSq);
        FireProjectile(new Vector3(dx * inv, 0f, dz * inv));
    }

    private void RollFireInterval() =>
        Owner.AiFireInterval = kFireIntervalMin +
                               (float)_rng.NextDouble() * (kFireIntervalMax - kFireIntervalMin);

    private void FireProjectile(Vector3 dir)
    {
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
