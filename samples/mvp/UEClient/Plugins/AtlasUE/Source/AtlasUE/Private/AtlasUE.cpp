#include "AtlasUE.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "entitydef/entitydef_api.h"
#include "net_client/client_api.h"

DEFINE_LOG_CATEGORY(LogAtlasUE);

namespace
{
// Loads a DLL from the plugin's ThirdParty dir with the surrounding directory
// pushed onto the search path so transitive deps resolve from the same folder.
void* LoadThirdPartyDll(const FString& BaseDir, const TCHAR* SubDir, const TCHAR* DllName)
{
	const FString DllDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(BaseDir, TEXT("ThirdParty"), SubDir, TEXT("Win64")));
	const FString DllPath = FPaths::Combine(DllDir, DllName);
	FPlatformProcess::PushDllDirectory(*DllDir);
	void* Handle = FPlatformProcess::GetDllHandle(*DllPath);
	FPlatformProcess::PopDllDirectory(*DllDir);
	if (Handle == nullptr)
	{
		UE_LOG(LogAtlasUE, Error, TEXT("failed to load %s at %s"), DllName, *DllPath);
	}
	return Handle;
}
}  // namespace

void FAtlasUEModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AtlasUE"));
	const FString BaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();

	NetClientDllHandle = LoadThirdPartyDll(BaseDir, TEXT("AtlasNetClient"), TEXT("atlas_net_client.dll"));
	EntityDefDllHandle = LoadThirdPartyDll(BaseDir, TEXT("AtlasEntityDef"), TEXT("atlas_entitydef_client.dll"));

	if (NetClientDllHandle == nullptr || EntityDefDllHandle == nullptr) return;

	const uint32 NetAbi = AtlasNetGetAbiVersion();
	const uint32 EdrAbi = AtlasEdrGetAbiVersion();
	UE_LOG(LogAtlasUE, Log,
		TEXT("AtlasUE module started; atlas_net_client ABI=0x%08X, atlas_entitydef_client ABI=0x%08X"),
		NetAbi, EdrAbi);
}

void FAtlasUEModule::ShutdownModule()
{
	if (EntityDefDllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(EntityDefDllHandle);
		EntityDefDllHandle = nullptr;
	}
	if (NetClientDllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(NetClientDllHandle);
		NetClientDllHandle = nullptr;
	}
	UE_LOG(LogAtlasUE, Log, TEXT("AtlasUE module stopped"));
}

IMPLEMENT_MODULE(FAtlasUEModule, AtlasUE)
