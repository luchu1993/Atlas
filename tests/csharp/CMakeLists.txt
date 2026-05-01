# C# test projects

atlas_dotnet_project(
  NAME atlas_smoke_test_dll
  PROJECT_FILE Atlas.SmokeTest/Atlas.SmokeTest.csproj
  ASSEMBLY_NAME Atlas.SmokeTest.dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_runtime_test_dll
  PROJECT_FILE Atlas.RuntimeTest/Atlas.RuntimeTest.csproj
  ASSEMBLY_NAME Atlas.RuntimeTest.dll
  DEPENDS atlas_runtime_dll
  DEPLOY
)

atlas_dotnet_project(
  NAME atlas_client_tests_dll
  PROJECT_FILE Atlas.Client.Tests/Atlas.Client.Tests.csproj
  ASSEMBLY_NAME Atlas.Client.Tests.dll
  DEPENDS atlas_shared_dll atlas_client_dll
)
