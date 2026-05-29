using Atlas.Mvp.Unity;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Atlas.Mvp.Editor
{
    // Seeds the MVP scene with the server collision obstacles the collision
    // pipeline demo expects: an invisible ground slab (replaces the implicit
    // flat ground that LoadCollisionAsset removes) plus a visible wall and
    // platform. Re-runnable; rebuilds the AtlasServerColliders root each time.
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
