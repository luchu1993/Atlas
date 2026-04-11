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

    protected override Assembly? Load(AssemblyName assemblyName)
    {
        var path = Path.Combine(_scriptDirectory, $"{assemblyName.Name}.dll");
        if (File.Exists(path))
            return LoadFromAssemblyPath(path);

        // Atlas.Runtime, Atlas.Shared, System.* — fall back to Default context
        return null;
    }
}
