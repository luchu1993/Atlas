using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Atlas.Client.IntegrationTests;

internal static partial class NativeLibraryResolver
{
    [ModuleInitializer]
    public static void Install()
    {
        var binDir = ResolveBinDir();
        if (binDir is null) return;

        // OS DLL search must locate atlas_test_helpers.dll's transitive
        // deps (TracyClient, mimalloc, atlas_engine). Setting PATH after
        // process start is too late on Windows; SetDllDirectory + the
        // PATH fallback covers Win + Linux.
        if (OperatingSystem.IsWindows())
        {
            SetDllDirectory(binDir);
        }
        else
        {
            var existing = Environment.GetEnvironmentVariable("LD_LIBRARY_PATH") ?? "";
            if (!existing.Contains(binDir, StringComparison.Ordinal))
                Environment.SetEnvironmentVariable("LD_LIBRARY_PATH", $"{binDir}:{existing}");
        }
    }

    [LibraryImport("kernel32.dll", EntryPoint = "SetDllDirectoryW", StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetDllDirectory(string path);

    private static string? ResolveBinDir()
    {
        var env = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR");
        if (!string.IsNullOrEmpty(env) && Directory.Exists(env)) return env;

        var dir = Path.GetDirectoryName(typeof(NativeLibraryResolver).Assembly.Location);
        for (int i = 0; i < 10 && dir is not null; i++)
        {
            var probe = Path.Combine(dir, "bin", "debug");
            if (Directory.Exists(probe) && File.Exists(Path.Combine(probe, "atlas_test_helpers.dll")))
                return probe;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }
}
