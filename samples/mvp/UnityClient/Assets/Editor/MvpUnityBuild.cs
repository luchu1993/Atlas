using System;
using System.Collections.Generic;
using System.Linq;
using UnityEditor;
using UnityEditor.Build.Reporting;

namespace Atlas.Mvp.Editor
{
    public static class MvpUnityBuild
    {
        const string kTargetArg = "-atlasBuildTarget";
        const string kOutputArg = "-atlasBuildOutput";
        const string kDevelopmentArg = "-atlasDevelopment";

        public static void BuildFromCommandLine()
        {
            try
            {
                var args = ParseArgs(Environment.GetCommandLineArgs());
                var targetName = Required(args, kTargetArg);
                var output = Required(args, kOutputArg);
                var (group, target) = ResolveTarget(targetName);
                var scenes = EditorBuildSettings.scenes
                    .Where(scene => scene.enabled)
                    .Select(scene => scene.path)
                    .ToArray();

                if (scenes.Length == 0) throw new InvalidOperationException("No enabled scenes in EditorBuildSettings");
                if (EditorUserBuildSettings.activeBuildTarget != target)
                    EditorUserBuildSettings.SwitchActiveBuildTarget(group, target);

                var options = BuildOptions.None;
                if (args.ContainsKey(kDevelopmentArg)) options |= BuildOptions.Development;

                UnityEngine.Debug.Log($"[MvpUnityBuild] target={targetName} output={output}");
                UnityEngine.Debug.Log($"[MvpUnityBuild] scenes={string.Join(", ", scenes)}");

                var report = BuildPipeline.BuildPlayer(new BuildPlayerOptions
                {
                    scenes = scenes,
                    locationPathName = output,
                    targetGroup = group,
                    target = target,
                    options = options,
                });

                var summary = report.summary;
                UnityEngine.Debug.Log(
                    $"[MvpUnityBuild] result={summary.result} errors={summary.totalErrors} " +
                    $"warnings={summary.totalWarnings} size={summary.totalSize} time={summary.totalTime}");

                EditorApplication.Exit(summary.result == BuildResult.Succeeded ? 0 : 1);
            }
            catch (Exception ex)
            {
                UnityEngine.Debug.LogError($"[MvpUnityBuild] {ex}");
                EditorApplication.Exit(1);
            }
        }

        static Dictionary<string, string> ParseArgs(string[] argv)
        {
            var args = new Dictionary<string, string>();
            for (int i = 0; i < argv.Length; ++i)
            {
                if (argv[i] == kDevelopmentArg)
                {
                    args[argv[i]] = "true";
                }
                else if ((argv[i] == kTargetArg || argv[i] == kOutputArg) && i + 1 < argv.Length)
                {
                    args[argv[i]] = argv[++i];
                }
            }
            return args;
        }

        static string Required(Dictionary<string, string> args, string key)
        {
            if (!args.TryGetValue(key, out var value) || string.IsNullOrWhiteSpace(value))
                throw new ArgumentException($"Missing {key}");
            return value;
        }

        static (BuildTargetGroup group, BuildTarget target) ResolveTarget(string target)
        {
            return target switch
            {
                "StandaloneWindows64" => (BuildTargetGroup.Standalone, BuildTarget.StandaloneWindows64),
                "StandaloneLinux64" => (BuildTargetGroup.Standalone, BuildTarget.StandaloneLinux64),
                "StandaloneOSX" => (BuildTargetGroup.Standalone, BuildTarget.StandaloneOSX),
                _ => throw new ArgumentException($"Unsupported build target: {target}"),
            };
        }
    }
}
