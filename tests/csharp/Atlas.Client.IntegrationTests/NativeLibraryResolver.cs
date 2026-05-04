using System;
using System.IO;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Atlas.Client.IntegrationTests;

internal static partial class NativeLibraryResolver
{
    private static readonly string[] AtlasNativeNames =
    {
        "atlas_test_helpers",
        "atlas_net_client",
    };

    [ModuleInitializer]
    public static void Install()
    {
        var binDir = ResolveBinDir();
        if (binDir is null) return;

        // Windows: SetDllDirectory teaches LoadLibrary about transitive deps
        // (TracyClient, mimalloc, atlas_engine) before any DllImport fires.
        if (OperatingSystem.IsWindows())
        {
            SetDllDirectory(binDir);
        }

        // Linux: LD_LIBRARY_PATH is read once at process startup, so setting
        // it now is too late. Hook the loader for any assembly's unresolved
        // native import — covers Atlas.Client.Desktop's atlas_net_client too.
        AssemblyLoadContext.Default.ResolvingUnmanagedDll += (assembly, name) =>
        {
            if (Array.IndexOf(AtlasNativeNames, name) < 0) return IntPtr.Zero;
            var path = Path.Combine(binDir, NativeFilename(name));
            return NativeLibrary.TryLoad(path, out var handle) ? handle : IntPtr.Zero;
        };
    }

    private static string NativeFilename(string name) =>
        OperatingSystem.IsWindows() ? $"{name}.dll" :
        OperatingSystem.IsMacOS()   ? $"lib{name}.dylib" :
                                      $"lib{name}.so";

    [LibraryImport("kernel32.dll", EntryPoint = "SetDllDirectoryW", StringMarshalling = StringMarshalling.Utf16)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetDllDirectory(string path);

    private static string? ResolveBinDir()
    {
        var env = Environment.GetEnvironmentVariable("ATLAS_BIN_DIR");
        if (!string.IsNullOrEmpty(env) && Directory.Exists(env)) return env;

        var dir = Path.GetDirectoryName(typeof(NativeLibraryResolver).Assembly.Location);
        var probeFile = NativeFilename(AtlasNativeNames[0]);
        for (int i = 0; i < 10 && dir is not null; i++)
        {
            var probe = Path.Combine(dir, "bin", "debug");
            if (Directory.Exists(probe) && File.Exists(Path.Combine(probe, probeFile)))
                return probe;
            dir = Path.GetDirectoryName(dir);
        }
        return null;
    }
}
