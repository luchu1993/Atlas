# C# projects built via dotnet CLI

atlas_dotnet_project(
  NAME atlas_shared_dll
  PROJECT_FILE Atlas.Shared/Atlas.Shared.csproj
  ASSEMBLY_NAME Atlas.Shared.dll
  TARGET_FRAMEWORK netstandard2.1
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_generators_def_dll
  PROJECT_FILE Atlas.Generators.Def/Atlas.Generators.Def.csproj
  ASSEMBLY_NAME Atlas.Generators.Def.dll
  TARGET_FRAMEWORK netstandard2.0
  # Roslyn analyzer — build-time only, no runtime deployment.
)

atlas_dotnet_project(
  NAME atlas_clrhost_dll
  PROJECT_FILE Atlas.ClrHost/Atlas.ClrHost.csproj
  ASSEMBLY_NAME Atlas.ClrHost.dll
  DEPENDS atlas_shared_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_runtime_dll
  PROJECT_FILE Atlas.Runtime/Atlas.Runtime.csproj
  ASSEMBLY_NAME Atlas.Runtime.dll
  DEPENDS atlas_shared_dll atlas_clrhost_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_client_dll
  PROJECT_FILE Atlas.Client/Atlas.Client.csproj
  ASSEMBLY_NAME Atlas.Client.dll
  TARGET_FRAMEWORK netstandard2.1
  DEPENDS atlas_shared_dll
  DEPLOY
)

# Atlas.Client.Desktop carries the CoreCLR-host-specific glue
# (UnmanagedCallersOnly bridges + P/Invoke to atlas_engine). Unity builds
# skip this target entirely — they ship Atlas.Client alongside their own
# P/Invoke layer against atlas_net_client.dll.
atlas_dotnet_project(
  NAME atlas_client_desktop_dll
  PROJECT_FILE Atlas.Client.Desktop/Atlas.Client.Desktop.csproj
  ASSEMBLY_NAME Atlas.Client.Desktop.dll
  DEPENDS atlas_shared_dll atlas_client_dll atlas_clrhost_dll
  DEPLOY
)

# Atlas.Tools.DefDump — offline build-time tool that extracts struct +
# entity blobs from a built server assembly and writes a binary
# descriptor container consumable by EntityDefRegistry::RegisterFromBinaryFile
# (DBApp's load path on processes that don't host CoreCLR).
#
# Not deployed via DEPLOY because dotnet console exes need the sibling
# `*.runtimeconfig.json` next to the DLL to bootstrap CoreCLR — the
# AtlasDotNetBuild deploy step only copies the .dll. Invoke this tool
# from its build directory:
#   src/csharp/Atlas.Tools.DefDump/bin/x64/<Config>/net9.0/Atlas.Tools.DefDump.exe
atlas_dotnet_project(
  NAME atlas_tools_defdump
  PROJECT_FILE Atlas.Tools.DefDump/Atlas.Tools.DefDump.csproj
  ASSEMBLY_NAME Atlas.Tools.DefDump.dll
  DEPENDS atlas_shared_dll
)

# Atlas.Tools.NetClientDemo — Phase 4 FFI verification harness for
# atlas_net_client.dll. Only built when ATLAS_BUILD_NET_CLIENT is ON.
# Run from its bin output, expects atlas_net_client.dll on PATH.
if(ATLAS_BUILD_NET_CLIENT)
  atlas_dotnet_project(
    NAME atlas_tools_net_client_demo
    PROJECT_FILE Atlas.Tools.NetClientDemo/Atlas.Tools.NetClientDemo.csproj
    ASSEMBLY_NAME Atlas.Tools.NetClientDemo.dll
    DEPENDS atlas_shared_dll atlas_client_dll
  )
endif()
