using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;

namespace Atlas.Hosting;

/// <summary>
/// Collectible AssemblyLoadContext for user game scripts.
/// User script assemblies are loaded here; engine/shared assemblies fall back to Default.
/// </summary>
internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    private readonly string _scriptDirectory;

    public ScriptLoadContext(string scriptDirectory)
        : base(isCollectible: true)
    {
        _scriptDirectory = scriptDirectory;
    }

    // Engine assemblies must be shared with the default ALC; otherwise the
    // script ALC gets its own fresh copy where static state (ThreadGuard's
    // main-thread id, native callback registrations, etc.) is zero, which
    // silently breaks [ModuleInitializer]-driven entity registration.
    // MSBuild copies Atlas.Runtime.dll and friends next to the user script
    // as normal build output; without this filter the override below would
    // find them on disk and load duplicates into the collectible ALC.
    private static readonly string[] EngineAssemblyPrefixes =
    {
        "Atlas.Runtime",
        "Atlas.Shared",
        "Atlas.Generators.",
    };

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        var name = assemblyName.Name ?? string.Empty;
        foreach (var prefix in EngineAssemblyPrefixes)
        {
            if (name == prefix || name.StartsWith(prefix + ".", StringComparison.Ordinal))
            {
                // Engine assemblies must be the SAME instance that the CLR
                // bootstrap loaded. hostfxr's load_assembly_and_get_function_pointer
                // uses an internal, non-Default ALC, so Atlas.Runtime ends
                // up there with its static state (ThreadGuard._mainThreadId,
                // NativeCallbacks, dispatcher tables) primed. Loading a
                // second copy by path gives the script a fresh
                // Atlas.Runtime with _mainThreadId=0 and every NativeApi
                // call throws InvalidOperationException in debug builds.
                // Scan ALL live ALCs for the already-loaded instance.
                foreach (var alc in AssemblyLoadContext.All)
                {
                    if (alc == this) continue;
                    foreach (var asm in alc.Assemblies)
                    {
                        if (asm.GetName().Name == name)
                            return asm;
                    }
                }
                // Not yet loaded anywhere — load into Default ALC from the
                // script directory (MSBuild copies engine assemblies here
                // as normal build output). This makes the script-ALC and
                // the bootstrap ALC share the same instance going forward.
                var enginePath = Path.Combine(_scriptDirectory, $"{name}.dll");
                if (File.Exists(enginePath))
                    return Default.LoadFromAssemblyPath(enginePath);
                return null;
            }
        }

        var path = Path.Combine(_scriptDirectory, $"{assemblyName.Name}.dll");
        if (File.Exists(path))
            return LoadFromAssemblyPath(path);

        // System.*, other framework assemblies — fall back to Default context
        return null;
    }
}
