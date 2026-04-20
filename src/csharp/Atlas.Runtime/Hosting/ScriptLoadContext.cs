using System;
using System.IO;
using System.Reflection;
using System.Runtime.Loader;
using Atlas.Entity;

namespace Atlas.Hosting;

/// <summary>
/// Collectible AssemblyLoadContext for user game scripts. Engine assemblies
/// (Atlas.Runtime, Atlas.Shared, Atlas.Generators.*) are redirected to
/// whichever ALC already holds them so every process shares one type
/// identity for ref-struct spans, delegate tables, and static state.
/// </summary>
internal sealed class ScriptLoadContext : AssemblyLoadContext
{
    private readonly string _scriptDirectory;

    public ScriptLoadContext(string scriptDirectory)
        : base(isCollectible: true)
    {
        _scriptDirectory = scriptDirectory;
    }

    // Engine assemblies must resolve to the instance already loaded by the
    // CLR bootstrap. If they were loaded a second time into the collectible
    // ALC the script would see fresh copies where static state (ThreadGuard's
    // main-thread id, NativeCallbacks registrations, dispatcher tables) is
    // zero. Worse, ref-struct method signatures (ref SpanWriter / SpanReader)
    // would reference a distinct type from what ServerEntity declares, so
    // override methods silently fail to bind and manifest as
    // TypeLoadException "method does not have an implementation".
    // MSBuild copies these DLLs next to the user script as normal build
    // output; without this filter the fallback below would find them on
    // disk and load duplicates into the wrong ALC.
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
                // Not yet loaded anywhere. We cannot use Default ALC as the
                // sink, because Atlas.Runtime is resolved by hostfxr into a
                // hostfxr-owned IsolatedComponentLoadContext (not Default).
                // If we load engine dependencies (e.g. Atlas.Shared) into
                // Default, they'll be a distinct type-system from the ones
                // Atlas.Runtime's own ALC eventually loads — and method
                // overrides with ref-struct parameters (SpanWriter /
                // SpanReader) silently fail to bind, manifesting as
                // TypeLoadException "method does not have an implementation".
                //
                // Force the load into Atlas.Runtime's ALC so everything
                // under the engine umbrella shares one type identity.
                var runtimeAsm = typeof(EntityFactory).Assembly;
                var runtimeAlc = AssemblyLoadContext.GetLoadContext(runtimeAsm);
                var enginePath = Path.Combine(_scriptDirectory, $"{name}.dll");
                if (runtimeAlc != null && File.Exists(enginePath))
                    return runtimeAlc.LoadFromAssemblyPath(enginePath);
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
