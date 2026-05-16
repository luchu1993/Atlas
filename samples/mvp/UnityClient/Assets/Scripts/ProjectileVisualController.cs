using System;
using System.Collections.Generic;
using Atlas.Mvp.Client;
using UnityEngine;
using AtlasVec = Atlas.DataTypes.Vector3;

namespace Atlas.Mvp.Unity
{
    public sealed class ProjectileVisualController : ITickable, IDisposable
    {
        // Mirrors the cell-side gravity so client + server arcs agree; the
        // lifetime cap is a safety net for missed OnProjectileEnded RPCs.
        const float kGravity = 12f;
        const float kMaxLifetime = 2.5f;
        const float kHitFxLifetime = 0.6f;

        sealed class Visual
        {
            public uint ShotId;
            public Vector3 Position;
            public Vector3 Velocity;
            public float Age;
            public GameObject Go = null!;
        }

        struct PendingHitFx
        {
            public GameObject Go;
            public float ReleaseTime;
        }

        readonly Dictionary<uint, Visual> _active = new();
        readonly List<uint> _stale = new();
        readonly Queue<GameObject> _projectilePool = new();
        readonly Queue<GameObject> _hitFxPool = new();
        readonly List<PendingHitFx> _pendingHitFx = new();
        readonly GameObject _root;

        public ProjectileVisualController()
        {
            _root = new GameObject("ProjectileRoot");
            ProjectileBus.Fired += OnFired;
            ProjectileBus.Ended += OnEnded;
            Ticker.Add(this);
        }

        public void Dispose()
        {
            Ticker.Remove(this);
            ProjectileBus.Fired -= OnFired;
            ProjectileBus.Ended -= OnEnded;
            if (_root != null) UnityEngine.Object.Destroy(_root);
        }

        void OnFired(uint shotId, uint ownerId, AtlasVec origin, AtlasVec velocity)
        {
            if (_active.ContainsKey(shotId)) return;
            var go = AcquireProjectile();
            go.name = $"Projectile_{shotId}";
            var pos = new Vector3(origin.X, origin.Y, origin.Z);
            go.transform.position = pos;
            _active[shotId] = new Visual
            {
                ShotId = shotId,
                Position = pos,
                Velocity = new Vector3(velocity.X, velocity.Y, velocity.Z),
                Go = go,
            };
        }

        void OnEnded(uint shotId, AtlasVec endPos, uint hitTargetId)
        {
            if (!_active.TryGetValue(shotId, out var v)) return;
            // Snap hit FX to server-authoritative endPos so damage origin matches.
            ReleaseProjectile(v.Go);
            _active.Remove(shotId);
            if (hitTargetId != 0) SpawnHitFx(new Vector3(endPos.X, endPos.Y, endPos.Z));
        }

        public void Tick(float dt)
        {
            _stale.Clear();
            foreach (var v in _active.Values)
            {
                v.Age += dt;
                v.Velocity.y -= kGravity * dt;
                v.Position += v.Velocity * dt;
                v.Go.transform.position = v.Position;
                if (v.Age > kMaxLifetime || v.Position.y <= 0f) _stale.Add(v.ShotId);
            }
            foreach (var id in _stale)
            {
                if (_active.TryGetValue(id, out var v)) ReleaseProjectile(v.Go);
                _active.Remove(id);
            }

            float now = Time.time;
            for (int i = _pendingHitFx.Count - 1; i >= 0; --i)
            {
                if (now < _pendingHitFx[i].ReleaseTime) continue;
                ReleaseHitFx(_pendingHitFx[i].Go);
                _pendingHitFx.RemoveAt(i);
            }
        }

        GameObject AcquireProjectile()
        {
            if (_projectilePool.Count > 0)
            {
                var go = _projectilePool.Dequeue();
                go.SetActive(true);
                return go;
            }
            return CreateProjectile();
        }

        void ReleaseProjectile(GameObject go)
        {
            if (go == null) return;
            go.SetActive(false);
            _projectilePool.Enqueue(go);
        }

        GameObject CreateProjectile()
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            go.transform.SetParent(_root.transform, false);
            go.transform.localScale = Vector3.one * 0.3f;
            if (go.TryGetComponent<Collider>(out var col)) UnityEngine.Object.Destroy(col);
            go.GetComponent<Renderer>().material.color = new Color(0.2f, 0.4f, 1f);
            return go;
        }

        void SpawnHitFx(Vector3 pos)
        {
            var fx = AcquireHitFx();
            fx.transform.position = pos;
            _pendingHitFx.Add(new PendingHitFx { Go = fx, ReleaseTime = Time.time + kHitFxLifetime });
        }

        GameObject AcquireHitFx()
        {
            GameObject go = _hitFxPool.Count > 0 ? _hitFxPool.Dequeue() : CreateHitFx();
            go.SetActive(true);
            var ps = go.GetComponent<ParticleSystem>();
            ps.Clear();
            ps.Play();
            return go;
        }

        void ReleaseHitFx(GameObject go)
        {
            if (go == null) return;
            go.SetActive(false);
            _hitFxPool.Enqueue(go);
        }

        GameObject CreateHitFx()
        {
            var go = new GameObject("HitFx");
            go.transform.SetParent(_root.transform, false);
            var ps = go.AddComponent<ParticleSystem>();
            ps.Stop(true, ParticleSystemStopBehavior.StopEmittingAndClear);

            var main = ps.main;
            main.duration = 0.05f;
            main.loop = false;
            main.startLifetime = 0.4f;
            main.startSpeed = 4f;
            main.startSize = 0.12f;
            main.startColor = new ParticleSystem.MinMaxGradient(
                new Color(1f, 0.85f, 0.25f), new Color(1f, 0.5f, 0.1f));
            main.gravityModifier = 2f;
            main.maxParticles = 40;
            main.simulationSpace = ParticleSystemSimulationSpace.World;
            main.playOnAwake = false;

            var emission = ps.emission;
            emission.rateOverTime = 0f;
            emission.SetBursts(new[] { new ParticleSystem.Burst(0f, 22) });

            var shape = ps.shape;
            shape.shapeType = ParticleSystemShapeType.Sphere;
            shape.radius = 0.05f;

            var sizeOverLifetime = ps.sizeOverLifetime;
            sizeOverLifetime.enabled = true;
            var sizeCurve = new AnimationCurve(
                new Keyframe(0f, 1f), new Keyframe(1f, 0f));
            sizeOverLifetime.size = new ParticleSystem.MinMaxCurve(1f, sizeCurve);

            var colorOverLifetime = ps.colorOverLifetime;
            colorOverLifetime.enabled = true;
            var grad = new Gradient();
            grad.SetKeys(
                new[]
                {
                    new GradientColorKey(new Color(1f, 0.95f, 0.5f), 0f),
                    new GradientColorKey(new Color(1f, 0.3f, 0.05f), 1f),
                },
                new[]
                {
                    new GradientAlphaKey(1f, 0f),
                    new GradientAlphaKey(0f, 1f),
                });
            colorOverLifetime.color = grad;

            var renderer = go.GetComponent<ParticleSystemRenderer>();
            renderer.material = new Material(Shader.Find("Sprites/Default"));
            return go;
        }
    }
}
