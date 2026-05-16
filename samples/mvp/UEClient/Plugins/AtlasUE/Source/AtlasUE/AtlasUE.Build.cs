using UnrealBuildTool;
using System.IO;

public class AtlasUE : ModuleRules
{
	public AtlasUE(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		CppStandard = CppStandardVersion.Cpp20;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects",
		});

		// atlas_net_client headers live in the Atlas repo. PluginDirectory is
		// .../samples/mvp/UEClient/Plugins/AtlasUE/, so the repo root is 5 levels up.
		string RepoRoot = Path.GetFullPath(Path.Combine(PluginDirectory, "..", "..", "..", "..", ".."));
		PublicSystemIncludePaths.Add(Path.Combine(RepoRoot, "src", "lib"));

		PublicDefinitions.Add("ATLAS_NET_CLIENT_DLL=1");

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string ThirdPartyWin64 = Path.Combine(PluginDirectory, "ThirdParty", "AtlasNetClient", "Win64");

			PublicAdditionalLibraries.Add(Path.Combine(ThirdPartyWin64, "atlas_net_client.lib"));
			PublicDelayLoadDLLs.Add("atlas_net_client.dll");

			RuntimeDependencies.Add(Path.Combine(ThirdPartyWin64, "atlas_net_client.dll"));
			RuntimeDependencies.Add(Path.Combine(ThirdPartyWin64, "mimalloc.dll"));
		}
	}
}
