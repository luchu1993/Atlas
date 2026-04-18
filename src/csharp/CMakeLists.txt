# C# projects built via dotnet CLI

atlas_dotnet_project(
  NAME atlas_shared_dll
  PROJECT_FILE Atlas.Shared/Atlas.Shared.csproj
  ASSEMBLY_NAME Atlas.Shared.dll
)

atlas_dotnet_project(
  NAME atlas_generators_events_dll
  PROJECT_FILE Atlas.Generators.Events/Atlas.Generators.Events.csproj
  ASSEMBLY_NAME Atlas.Generators.Events.dll
)

atlas_dotnet_project(
  NAME atlas_runtime_dll
  PROJECT_FILE Atlas.Runtime/Atlas.Runtime.csproj
  ASSEMBLY_NAME Atlas.Runtime.dll
  DEPENDS atlas_shared_dll atlas_generators_events_dll
)
