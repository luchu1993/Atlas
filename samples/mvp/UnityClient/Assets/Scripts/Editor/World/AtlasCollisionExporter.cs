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
            public int Spheres;
            public int Capsules;
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
                else if (string.IsNullOrEmpty(SceneManager.GetActiveScene().path))
                {
                    // Batch mode boots an untitled empty scene; exporting it would
                    // silently write zero objects. Demand an explicit scene instead.
                    throw new InvalidOperationException(
                        "No -atlasExportScene given and the active scene is untitled; " +
                        "pass --scene <Assets/.../X.unity> to export a saved scene.");
                }

                var result = ExportActiveSceneToFile(output, sourceHash);
                Debug.Log($"[AtlasCollisionExporter] boxes={result.Boxes} spheres={result.Spheres} " +
                          $"capsules={result.Capsules} skipped={result.Skipped} output={output}");
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
                        if (TryEmitBox(box, auth, objects, ref first, result)) result.Boxes++;
                    }
                    else if (col is SphereCollider sphere)
                    {
                        if (TryEmitSphere(sphere, auth, objects, ref first, result)) result.Spheres++;
                    }
                    else if (col is CapsuleCollider capsule)
                    {
                        if (TryEmitCapsule(capsule, auth, objects, ref first, result)) result.Capsules++;
                    }
                    else
                    {
                        result.Skipped++;
                        result.Warnings.Add($"{Trace(go)}: {col.GetType().Name} not supported by exporter (box / sphere / capsule only)");
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

        // Sphere / capsule radius can't survive non-uniform scale (it would become
        // an ellipsoid); returns false and warns when the scale isn't uniform.
        static bool TryUniformScale(Transform t, string what, ExportResult result, out float scale)
        {
            var s = t.lossyScale;
            scale = s.x;
            if (s.x <= 0f || s.y <= 0f || s.z <= 0f)
            {
                result.Skipped++;
                result.Warnings.Add($"{Trace(t.gameObject)}: lossyScale {s} non-positive, skip");
                return false;
            }
            if (!Mathf.Approximately(s.x, s.y) || !Mathf.Approximately(s.x, s.z))
            {
                result.Skipped++;
                result.Warnings.Add($"{Trace(t.gameObject)}: {what} needs uniform scale, got {s}, skip");
                return false;
            }
            return true;
        }

        static bool TryEmitSphere(SphereCollider sphere, ServerColliderAuthoring auth,
                                  StringBuilder objects, ref bool first, ExportResult result)
        {
            var t = sphere.transform;
            if (!TryUniformScale(t, "SphereCollider", result, out var scale)) return false;

            var c = t.TransformPoint(sphere.center);
            var radius = sphere.radius * scale;
            if (!first) objects.Append(",");
            first = false;
            objects.Append("\n    ");
            objects.AppendFormat(CultureInfo.InvariantCulture,
                "{{\"shape\": \"sphere\", \"center\": [{0:G9}, {1:G9}, {2:G9}], \"radius\": {3:G9}, \"layer\": {4}}}",
                c.x, c.y, c.z, radius, auth.layer);
            return true;
        }

        static bool TryEmitCapsule(CapsuleCollider capsule, ServerColliderAuthoring auth,
                                   StringBuilder objects, ref bool first, ExportResult result)
        {
            // Atlas StaticCapsule is vertical; only Y-axis Unity capsules map cleanly.
            if (capsule.direction != 1)
            {
                result.Skipped++;
                result.Warnings.Add($"{Trace(capsule.gameObject)}: CapsuleCollider direction must be Y-axis, skip");
                return false;
            }
            var t = capsule.transform;
            if (!TryUniformScale(t, "CapsuleCollider", result, out var scale)) return false;

            var c = t.TransformPoint(capsule.center);
            var radius = capsule.radius * scale;
            // Unity height includes the caps; clamp so it never under-runs a sphere.
            var height = Mathf.Max(capsule.height, 2f * capsule.radius) * scale;
            var halfHeight = height * 0.5f;
            if (!first) objects.Append(",");
            first = false;
            objects.Append("\n    ");
            objects.AppendFormat(CultureInfo.InvariantCulture,
                "{{\"shape\": \"capsule\", \"center\": [{0:G9}, {1:G9}, {2:G9}], \"radius\": {3:G9}, \"half_height\": {4:G9}, \"layer\": {5}}}",
                c.x, c.y, c.z, radius, halfHeight, auth.layer);
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
