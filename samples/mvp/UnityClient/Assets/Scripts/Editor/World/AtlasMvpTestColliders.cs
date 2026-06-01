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
