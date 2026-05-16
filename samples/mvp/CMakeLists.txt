# MVP demo script assemblies. Loaded by BaseApp / CellApp when
# tools/bin/run_mvp_cluster brings the cluster up.

atlas_dotnet_project(
  NAME atlas_mvp_base_dll
  PROJECT_FILE Atlas.Mvp.Base/Atlas.Mvp.Base.csproj
  ASSEMBLY_NAME Atlas.Mvp.Base.dll
  DEPENDS atlas_runtime_dll atlas_generators_def_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_mvp_cell_dll
  PROJECT_FILE Atlas.Mvp.Cell/Atlas.Mvp.Cell.csproj
  ASSEMBLY_NAME Atlas.Mvp.Cell.dll
  DEPENDS atlas_runtime_dll atlas_generators_def_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_mvp_client_dll
  PROJECT_FILE Atlas.Mvp.Client/Atlas.Mvp.Client.csproj
  ASSEMBLY_NAME Atlas.Mvp.Client.dll
  DEPENDS atlas_client_dll atlas_client_desktop_dll atlas_clrhost_dll atlas_generators_def_dll
  DEPLOY
)
