# C# projects built via dotnet CLI

atlas_dotnet_project(
  NAME atlas_shared_dll
  PROJECT_FILE Atlas.Shared/Atlas.Shared.csproj
  ASSEMBLY_NAME Atlas.Shared.dll
  TARGET_FRAMEWORK netstandard2.1
  DEPLOY_TO server client tools
)

atlas_dotnet_project(
  NAME atlas_generators_events_dll
  PROJECT_FILE Atlas.Generators.Events/Atlas.Generators.Events.csproj
  ASSEMBLY_NAME Atlas.Generators.Events.dll
  TARGET_FRAMEWORK netstandard2.0
  # Roslyn analyzer — build-time only, no runtime deployment.
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
  DEPLOY_TO server client tools
)

atlas_dotnet_project(
  NAME atlas_runtime_dll
  PROJECT_FILE Atlas.Runtime/Atlas.Runtime.csproj
  ASSEMBLY_NAME Atlas.Runtime.dll
  DEPENDS atlas_shared_dll atlas_clrhost_dll atlas_generators_events_dll
  DEPLOY_TO server tools
)

atlas_dotnet_project(
  NAME atlas_client_dll
  PROJECT_FILE Atlas.Client/Atlas.Client.csproj
  ASSEMBLY_NAME Atlas.Client.dll
  TARGET_FRAMEWORK netstandard2.1
  DEPENDS atlas_shared_dll
  DEPLOY_TO client
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
  DEPLOY_TO client
)
