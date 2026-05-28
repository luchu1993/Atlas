using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using Atlas.Mvp.Unity;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;
using UnityEngine.SceneManagement;

namespace Atlas.Mvp.Editor
{
    public static class AtlasCollisionExporter
    {
        const string kOutputArg = "-atlasExportOutput";
        const string kSourceHashArg = "-atlasExportSourceHash";
        const string kSceneArg = "-atlasExportScene";
        const float kRotationEpsilon = 1e-3f;

        public sealed class ExportResult
        {
            public int Boxes;
            public int Skipped;
            public List<string> Warnings = new();
        }

        // CLI entry point invoked via Unity -executeMethod.
        public static void ExportFromCommandLine()
        {
            try
            {
                var args = ParseArgs(Environment.GetCommandLineArgs());
                if (!args.TryGetValue(kOutputArg, out var output))
                    throw new ArgumentException($"Missing {kOutputArg}");
                args.TryGetValue(kSourceHashArg, out var sourceHash);
                args.TryGetValue(kSceneArg, out var scenePath);

                if (!string.IsNullOrEmpty(scenePath))
                {
                    EditorSceneManager.OpenScene(scenePath, OpenSceneMode.Single);
                }

                var result = ExportActiveSceneToFile(output, sourceHash);
                Debug.Log($"[AtlasCollisionExporter] boxes={result.Boxes} skipped={result.Skipped} output={output}");
                foreach (var w in result.Warnings) Debug.LogWarning($"[AtlasCollisionExporter] {w}");
                EditorApplication.Exit(0);
            }
            catch (Exception ex)
            {
                Debug.LogError($"[AtlasCollisionExporter] {ex}");
                EditorApplication.Exit(1);
            }
        }

        // Programmatic API: scan the active scene and write a v2 collision asset.
        public static ExportResult ExportActiveSceneToFile(string outputPath, string sourceHash)
        {
            if (string.IsNullOrWhiteSpace(outputPath))
                throw new ArgumentException("outputPath must be non-empty");

            var scene = SceneManager.GetActiveScene();
            if (!scene.IsValid())
                throw new InvalidOperationException("No active scene to export");
            if (string.IsNullOrEmpty(sourceHash))
                sourceHash = DefaultSourceHash(scene);

            var roots = scene.GetRootGameObjects();
            var (json, result) = BuildJson(roots, sourceHash);

            var outFile = Path.GetFullPath(outputPath);
            Directory.CreateDirectory(Path.GetDirectoryName(outFile) ?? ".");
            File.WriteAllText(outFile, json, new UTF8Encoding(false));
            return result;
        }

        // Pure helper: returns the JSON document and a summary. No I/O. Tests call this directly.
        public static (string Json, ExportResult Result) BuildJson(IEnumerable<GameObject> roots,
                                                                    string sourceHash)
        {
            var result = new ExportResult();
            var objects = new StringBuilder();
            bool first = true;

            foreach (var root in roots)
            {
                var authorings = root.GetComponentsInChildren<ServerColliderAuthoring>(includeInactive: false);
                foreach (var auth in authorings)
                {
                    if (!auth.exportToServer) continue;

                    var go = auth.gameObject;
                    var col = go.GetComponent<Collider>();
                    if (col == null)
                    {
                        result.Skipped++;
                        result.Warnings.Add($"{Trace(go)}: ServerColliderAuthoring with no Collider");
                        continue;
                    }

                    if (col is BoxCollider box)
                    {
                        var emitted = TryEmitBox(box, auth, objects, ref first, result);
                        if (emitted) result.Boxes++;
                    }
                    else
                    {
                        result.Skipped++;
                        result.Warnings.Add($"{Trace(go)}: {col.GetType().Name} not supported by exporter (only BoxCollider)");
                    }
                }
            }

            var doc = new StringBuilder();
            doc.Append("{\n");
            doc.Append("  \"version\": 2,\n");
            doc.Append("  \"coordinate_system\": \"x_right_y_up_z_forward_meters\",\n");
            doc.AppendFormat("  \"source_hash\": \"{0}\",\n", EscapeJson(sourceHash));
            doc.Append("  \"objects\": [");
            doc.Append(objects);
            doc.Append(first ? "]\n" : "\n  ]\n");
            doc.Append("}\n");
            return (doc.ToString(), result);
        }

        static bool TryEmitBox(BoxCollider box, ServerColliderAuthoring auth, StringBuilder objects,
                                ref bool first, ExportResult result)
        {
            var t = box.transform;
            var rot = t.rotation;
            var ident = Quaternion.identity;
            // angle returns 0..180; close to 0 means aligned with world axes.
            var angle = Quaternion.Angle(rot, ident);
            if (angle > kRotationEpsilon)
            {
                result.Skipped++;
                result.Warnings.Add($"{Trace(box.gameObject)}: BoxCollider rotated by {angle:F2}° — Atlas StaticBox is AABB, skip");
                return false;
            }

            var scale = t.lossyScale;
            if (scale.x <= 0f || scale.y <= 0f || scale.z <= 0f)
            {
                result.Skipped++;
                result.Warnings.Add($"{Trace(box.gameObject)}: lossyScale {scale} has non-positive component, skip");
                return false;
            }

            var worldCenter = t.position + Vector3.Scale(box.center, scale);
            var halfExtent = Vector3.Scale(box.size, scale) * 0.5f;
            var min = worldCenter - halfExtent;
            var max = worldCenter + halfExtent;

            if (!first) objects.Append(",");
            first = false;
            objects.Append("\n    ");
            objects.AppendFormat(CultureInfo.InvariantCulture,
                "{{\"shape\": \"box\", \"min\": [{0:G9}, {1:G9}, {2:G9}], \"max\": [{3:G9}, {4:G9}, {5:G9}], \"layer\": {6}}}",
                min.x, min.y, min.z, max.x, max.y, max.z, auth.layer);
            return true;
        }

        static Dictionary<string, string> ParseArgs(string[] argv)
        {
            var args = new Dictionary<string, string>();
            for (int i = 0; i < argv.Length - 1; ++i)
            {
                if (argv[i] == kOutputArg || argv[i] == kSourceHashArg || argv[i] == kSceneArg)
                {
                    args[argv[i]] = argv[i + 1];
                }
            }
            return args;
        }

        static string DefaultSourceHash(Scene scene)
        {
            var name = string.IsNullOrEmpty(scene.path) ? scene.name : scene.path;
            return $"unity:{name}";
        }

        static string Trace(GameObject go)
        {
            var stack = new Stack<string>();
            for (var t = go.transform; t != null; t = t.parent) stack.Push(t.name);
            return string.Join("/", stack);
        }

        static string EscapeJson(string s)
        {
            return s.Replace("\\", "\\\\").Replace("\"", "\\\"");
        }
    }
}
