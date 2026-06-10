using Atlas.Mvp.Unity;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Atlas.Mvp.Editor
{
    // Rebuilds the AtlasServerColliders root with one obstacle per exportable
    // server collision shape, so the export → cook → load pipeline has inputs.
    public static class AtlasMvpTestColliders
    {
        const string kRootName = "AtlasServerColliders";
        const string kScenePath = "Assets/Scenes/Main.unity";

        [MenuItem("Atlas/MVP/Seed Server Test Colliders")]
        public static void SeedInteractive()
        {
            if (!SceneManager.GetActiveScene().path.EndsWith("Main.unity"))
                EditorSceneManager.OpenScene(kScenePath, OpenSceneMode.Single);
            Seed();
            EditorSceneManager.SaveScene(SceneManager.GetActiveScene());
        }

        // Batch entry point: open Main.unity, seed, save.
        public static void SeedFromCommandLine()
        {
            try
            {
                EditorSceneManager.OpenScene(kScenePath, OpenSceneMode.Single);
                Seed();
                EditorSceneManager.SaveScene(SceneManager.GetActiveScene());
                Debug.Log("[AtlasMvpTestColliders] seeded server colliders into Main.unity");
                EditorApplication.Exit(0);
            }
            catch (System.Exception ex)
            {
                Debug.LogError($"[AtlasMvpTestColliders] {ex}");
                EditorApplication.Exit(1);
            }
        }

        static void Seed()
        {
            var existing = GameObject.Find(kRootName);
            if (existing != null) Object.DestroyImmediate(existing);

            var root = new GameObject(kRootName);
            // Ground slab covering the 200 x 200 m play area, top at y=0; invisible
            // so it doesn't fight the scene's existing visual floor.
            AddBox(root, "Ground", new Vector3(0f, -0.5f, 0f), new Vector3(200f, 1f, 200f),
                   layer: 0, visible: false);
            // A wall to walk into and a platform to step onto — both visible.
            AddBox(root, "Wall", new Vector3(5.5f, 1.5f, 0f), new Vector3(1f, 3f, 10f),
                   layer: 0, visible: true);
            AddBox(root, "Platform", new Vector3(-6f, 0.75f, -6f), new Vector3(4f, 1.5f, 4f),
                   layer: 0, visible: true);
            // A sphere boulder and a capsule pillar exercise the non-box shapes.
            AddPrimitive(root, "Boulder", PrimitiveType.Sphere, new Vector3(8f, 1f, 4f),
                         uniformScale: 2f, layer: 0);
            AddPrimitive(root, "Pillar", PrimitiveType.Capsule, new Vector3(-6f, 1f, 6f),
                         uniformScale: 1f, layer: 0);
            // A Quad carries a MeshCollider — a vertical mesh wall (collision is
            // double-sided, so orientation/walkability isn't a concern here).
            AddMeshWall(root, "MeshWall", new Vector3(0f, 1.5f, 10f), new Vector3(6f, 3f, 1f),
                        layer: 0);
            // A convex MeshCollider crate exercises the convex-hull export path.
            AddConvexCrate(root, "ConvexCrate", new Vector3(10f, 1f, -8f), 2f, layer: 0);
            // A small Terrain exercises the heightfield export path.
            AddTerrain(root, "Terrain", new Vector3(40f, 0f, -40f),
                       new Vector3(50f, 5f, 50f), resolution: 33, layer: 0);
            SeedWhitebox(root);
        }

        // OW-style PVP whitebox (perimeter wall + doorways, high ground + stairs,
        // covers, flank corridors); mirrors samples/mvp/maps/main.collision.json.
        static void SeedWhitebox(GameObject root)
        {
            var boxes = new (string Name, Vector3 Center, Vector3 Size)[]
            {
                ("WallNW", new Vector3(-21.5f, 1.5f, 40f), new Vector3(37f, 3f, 0.8f)),
                ("WallNE", new Vector3(21.5f, 1.5f, 40f), new Vector3(37f, 3f, 0.8f)),
                ("WallSW", new Vector3(-21.5f, 1.5f, -40f), new Vector3(37f, 3f, 0.8f)),
                ("WallSE", new Vector3(21.5f, 1.5f, -40f), new Vector3(37f, 3f, 0.8f)),
                ("WallES", new Vector3(40f, 1.5f, -21.5f), new Vector3(0.8f, 3f, 37f)),
                ("WallEN", new Vector3(40f, 1.5f, 21.5f), new Vector3(0.8f, 3f, 37f)),
                ("WallWS", new Vector3(-40f, 1.5f, -21.5f), new Vector3(0.8f, 3f, 37f)),
                ("WallWN", new Vector3(-40f, 1.5f, 21.5f), new Vector3(0.8f, 3f, 37f)),
                ("HighGround", new Vector3(0f, 1f, 16f), new Vector3(16f, 2f, 16f)),
                ("CoverWS", new Vector3(-12.5f, 0.75f, -1.7f), new Vector3(3f, 1.5f, 0.6f)),
                ("CoverES", new Vector3(12.5f, 0.75f, -1.7f), new Vector3(3f, 1.5f, 0.6f)),
                ("CoverWN", new Vector3(-12.5f, 0.75f, 4.3f), new Vector3(3f, 1.5f, 0.6f)),
                ("CoverEN", new Vector3(12.5f, 0.75f, 4.3f), new Vector3(3f, 1.5f, 0.6f)),
                ("CoverMid", new Vector3(0f, 1f, -11.25f), new Vector3(3f, 2f, 1.5f)),
                ("FlankWOuter", new Vector3(-25.6f, 1.25f, -4f), new Vector3(0.8f, 2.5f, 32f)),
                ("FlankWInner", new Vector3(-21.6f, 1.25f, 2f), new Vector3(0.8f, 2.5f, 32f)),
                ("FlankEOuter", new Vector3(25.6f, 1.25f, -4f), new Vector3(0.8f, 2.5f, 32f)),
                ("FlankEInner", new Vector3(21.6f, 1.25f, 2f), new Vector3(0.8f, 2.5f, 32f)),
            };
            foreach (var (name, center, size) in boxes)
                AddBox(root, name, center, size, layer: 0, visible: true);
            // Stair ramps onto the high ground; 0.4 m risers stay under the
            // movement step-up and the nav agent_max_climb (0.45 m).
            for (int i = 1; i <= 5; ++i)
            {
                var size = new Vector3(4f, 0.4f * i, 1f);
                var y = 0.2f * i;
                var z = 2.5f + i;  // riser height grows toward the high ground at z=8
                AddBox(root, $"StairW{i}", new Vector3(-4f, y, z), size, layer: 0, visible: true);
                AddBox(root, $"StairE{i}", new Vector3(4f, y, z), size, layer: 0, visible: true);
            }
        }

        const string kTerrainDataPath = "Assets/Scenes/MvpTestTerrain.asset";

        static void AddTerrain(GameObject parent, string name, Vector3 origin, Vector3 size,
                               int resolution, int layer)
        {
            AssetDatabase.DeleteAsset(kTerrainDataPath);  // re-runnable
            var data = new TerrainData();
            data.heightmapResolution = resolution;  // 2^k+1; must precede SetHeights
            data.size = size;
            var heights = new float[resolution, resolution];
            for (int z = 0; z < resolution; ++z)
            {
                for (int x = 0; x < resolution; ++x)
                {
                    float dx = x / (float)(resolution - 1) - 0.5f;
                    float dz = z / (float)(resolution - 1) - 0.5f;
                    heights[z, x] = Mathf.Max(0f, 0.6f - Mathf.Sqrt(dx * dx + dz * dz));  // dome
                }
            }
            data.SetHeights(0, 0, heights);
            AssetDatabase.CreateAsset(data, kTerrainDataPath);

            var go = Terrain.CreateTerrainGameObject(data);  // adds Terrain + TerrainCollider
            go.name = name;
            go.transform.SetParent(parent.transform, worldPositionStays: true);
            go.transform.position = origin;
            var authoring = go.AddComponent<ServerColliderAuthoring>();
            authoring.exportToServer = true;
            authoring.layer = layer;
        }

        static void AddConvexCrate(GameObject parent, string name, Vector3 center,
                                   float scale, int layer)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Cube);
            go.name = name;
            // Swap the primitive BoxCollider for a convex MeshCollider on the cube mesh.
            Object.DestroyImmediate(go.GetComponent<BoxCollider>());
            var mc = go.AddComponent<MeshCollider>();
            mc.sharedMesh = go.GetComponent<MeshFilter>().sharedMesh;
            mc.convex = true;
            go.transform.SetParent(parent.transform, worldPositionStays: true);
            go.transform.position = center;
            go.transform.localScale = new Vector3(scale, scale, scale);
            var authoring = go.AddComponent<ServerColliderAuthoring>();
            authoring.exportToServer = true;
            authoring.layer = layer;
        }

        static void AddMeshWall(GameObject parent, string name, Vector3 center, Vector3 scale,
                                int layer)
        {
            var go = GameObject.CreatePrimitive(PrimitiveType.Quad);  // Quad → MeshCollider
            go.name = name;
            go.transform.SetParent(parent.transform, worldPositionStays: true);
            go.transform.position = center;
            go.transform.localScale = scale;
            var authoring = go.AddComponent<ServerColliderAuthoring>();
            authoring.exportToServer = true;
            authoring.layer = layer;
        }

        static void AddPrimitive(GameObject parent, string name, PrimitiveType type,
                                 Vector3 center, float uniformScale, int layer)
        {
            var go = GameObject.CreatePrimitive(type);
            go.name = name;
            go.transform.SetParent(parent.transform, worldPositionStays: true);
            go.transform.position = center;
            go.transform.localScale = new Vector3(uniformScale, uniformScale, uniformScale);
            var authoring = go.AddComponent<ServerColliderAuthoring>();
            authoring.exportToServer = true;
            authoring.layer = layer;
        }

        static void AddBox(GameObject parent, string name, Vector3 center, Vector3 size,
                           int layer, bool visible)
        {
            GameObject go;
            if (visible)
            {
                go = GameObject.CreatePrimitive(PrimitiveType.Cube);
                go.name = name;
            }
            else
            {
                go = new GameObject(name);
                go.AddComponent<BoxCollider>();
            }
            go.transform.SetParent(parent.transform, worldPositionStays: true);
            go.transform.position = center;
            go.transform.localScale = size;  // unit-cube collider → world box = center ± size/2

            var authoring = go.AddComponent<ServerColliderAuthoring>();
            authoring.exportToServer = true;
            authoring.layer = layer;
        }
    }
}
