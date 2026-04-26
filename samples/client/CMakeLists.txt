# Client sample assembly, loaded by atlas_client via hostfxr at startup.
atlas_dotnet_project(
  NAME atlas_client_sample_dll
  PROJECT_FILE Atlas.ClientSample.csproj
  ASSEMBLY_NAME Atlas.ClientSample.dll
  DEPENDS atlas_client_dll atlas_client_desktop_dll atlas_clrhost_dll
  DEPLOY_TO client
)
