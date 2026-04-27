# Stress-test script assemblies, loaded by BaseApp / CellApp when
# run_world_stress.py spawns the cluster.
#
# Each .csproj sets exactly one of ATLAS_BASE / ATLAS_CELL so DefGenerator
# emits the correct process-side dispatch. Both assemblies include BOTH
# defs so the 1-based alphabetical type-id assignment stays consistent
# across processes.

atlas_dotnet_project(
  NAME atlas_stress_test_base_dll
  PROJECT_FILE Atlas.StressTest.Base/Atlas.StressTest.Base.csproj
  ASSEMBLY_NAME Atlas.StressTest.Base.dll
  DEPENDS atlas_runtime_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_stress_test_cell_dll
  PROJECT_FILE Atlas.StressTest.Cell/Atlas.StressTest.Cell.csproj
  ASSEMBLY_NAME Atlas.StressTest.Cell.dll
  DEPENDS atlas_runtime_dll
  DEPLOY
)
