using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Atlas.Client.Tests;

internal static class NativeLibraryResolver
{
    private const string AtlasNetClient = "atlas_net_client";

    [ModuleInitializer]
    public static void Install()
    {
        var binDir = ResolveBinDir();
        if (binDir is null) return;

        if (OperatingSystem.IsWindows())
        {
            SetDllDirectory(binDir);
        }

        AssemblyLoadContext.Default.ResolvingUnmanagedDll += (_, name) =>
        {
            if (name != AtlasNetClient) return IntPtr.Zero;
            var path = Path.Combine(binDir, NativeFilename(name));
            return NativeLibrary.TryLoad(path, out var handle) ? handle : IntPtr.Zero;
        };
    }

    private static string NativeFilename(string name) =>
        OperatingSystem.IsWindows() ? $"{name}.dll" :
        OperatingSystem.IsMacOS() ? $"lib{name}.dylib" :
        $"lib{name}.so";

    private static string? ResolveBinDir()
    {
        var env = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR");
        if (!string.IsNullOrEmpty(env) && Directory.Exists(env)) return env;

        var dir = Path.GetDirectoryName(typeof(NativeLibraryResolver).Assembly.Location);
        for (int i = 0; i < 10 && dir is not null; ++i)
        {
            var probe = Path.Combine(dir, "bin", "debug");
            if (File.Exists(Path.Combine(probe, NativeFilename(AtlasNetClient)))) return probe;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }

    [DllImport("kernel32.dll", EntryPoint = "SetDllDirectoryW", CharSet = CharSet.Unicode)]
    private static extern bool SetDllDirectory(string path);
}
