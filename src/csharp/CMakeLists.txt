# C# projects built via dotnet CLI

atlas_dotnet_project(
  NAME atlas_shared_dll
  PROJECT_FILE Atlas.Shared/Atlas.Shared.csproj
  ASSEMBLY_NAME Atlas.Shared.dll
  TARGET_FRAMEWORK netstandard2.1
)

atlas_dotnet_project(
  NAME atlas_generators_events_dll
  PROJECT_FILE Atlas.Generators.Events/Atlas.Generators.Events.csproj
  ASSEMBLY_NAME Atlas.Generators.Events.dll
  TARGET_FRAMEWORK netstandard2.0
)

atlas_dotnet_project(
  NAME atlas_generators_def_dll
  PROJECT_FILE Atlas.Generators.Def/Atlas.Generators.Def.csproj
  ASSEMBLY_NAME Atlas.Generators.Def.dll
  TARGET_FRAMEWORK netstandard2.0
)

atlas_dotnet_project(
  NAME atlas_runtime_dll
  PROJECT_FILE Atlas.Runtime/Atlas.Runtime.csproj
  ASSEMBLY_NAME Atlas.Runtime.dll
  DEPENDS atlas_shared_dll atlas_generators_events_dll
)

atlas_dotnet_project(
  NAME atlas_client_dll
  PROJECT_FILE Atlas.Client/Atlas.Client.csproj
  ASSEMBLY_NAME Atlas.Client.dll
  DEPENDS atlas_shared_dll
)
