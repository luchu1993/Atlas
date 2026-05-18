using System.Collections.Generic;

namespace Atlas.Generators.Def;

internal sealed class ComponentIdManifest
{
    public string SourcePath { get; }
    public Dictionary<string, ushort> ActiveByName { get; }
    public HashSet<ushort> DeprecatedIds { get; }
    public HashSet<string> DeprecatedNames { get; }

    public ComponentIdManifest(string sourcePath,
                                Dictionary<string, ushort> activeByName,
                                HashSet<ushort> deprecatedIds,
                                HashSet<string> deprecatedNames)
    {
        SourcePath = sourcePath;
        ActiveByName = activeByName;
        DeprecatedIds = deprecatedIds;
        DeprecatedNames = deprecatedNames;
    }
}
