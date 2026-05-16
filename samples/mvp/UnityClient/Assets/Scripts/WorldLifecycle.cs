using System;
using Atlas.Client;
using Atlas.Client.Unity;
using UnityEngine;

namespace Atlas.Mvp.Unity
{
    public sealed class WorldLifecycle : IDisposable
    {
        readonly AtlasNetworkManager _net;
        readonly Material? _groundMaterial;
        GameObject? _worldRoot;
        GameHud? _hud;
        ProjectileVisualController? _projectiles;

        public Transform? WorldRoot => _worldRoot != null ? _worldRoot.transform : null;
        public GameHud? Hud => _hud;
        public bool IsBuilt => _worldRoot != null;

        public WorldLifecycle(AtlasNetworkManager net, Material? groundMaterial)
        {
            _net = net;
            _groundMaterial = groundMaterial;
        }

        public void Build()
        {
            if (_worldRoot != null) return;
            _worldRoot = new GameObject("World");

            var ground = GameObject.CreatePrimitive(PrimitiveType.Plane);
            ground.name = "Ground";
            ground.transform.SetParent(_worldRoot.transform, false);
            // Primitive plane is 10m; scale 20× → 200m world.
            ground.transform.localScale = new Vector3(20f, 1f, 20f);
            var renderer = ground.GetComponent<Renderer>();
            if (_groundMaterial != null)
            {
                renderer.material = _groundMaterial;
                renderer.material.mainTextureScale = new Vector2(100f, 100f);
            }
            else
            {
                renderer.material.color = new Color(0.55f, 0.55f, 0.55f);
            }

            var lightGo = new GameObject("DirectionalLight");
            lightGo.transform.SetParent(_worldRoot.transform, false);
            var light = lightGo.AddComponent<Light>();
            light.type = LightType.Directional;
            light.intensity = 1.0f;
            lightGo.transform.rotation = Quaternion.Euler(50f, -30f, 0f);

            var hudGo = new GameObject("HudRoot");
            hudGo.transform.SetParent(_worldRoot.transform, false);
            _hud = hudGo.AddComponent<GameHud>();
            _hud.Bind(_net);

            LabelOverlay.Init();
            _projectiles = new ProjectileVisualController();
        }

        public void Dispose()
        {
            if (_worldRoot == null) return;
            _hud = null;
            _projectiles?.Dispose();
            _projectiles = null;
            AoiBoxes.Clear();
            LabelOverlay.Shutdown();
            // net_client fires no entity-destroyed callbacks on disconnect;
            // clear the SDK manager to avoid ghosts on a fast re-login.
            ClientCallbacks.EntityManager.Clear();
            UnityEngine.Object.Destroy(_worldRoot);
            _worldRoot = null;
        }
    }
}
