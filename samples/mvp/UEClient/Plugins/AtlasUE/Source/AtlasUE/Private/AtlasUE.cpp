#include "AtlasUE.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "net_client/client_api.h"

DEFINE_LOG_CATEGORY(LogAtlasUE);

void FAtlasUEModule::StartupModule()
{
	// Explicit load via the plugin's ThirdParty dir so the editor's PATH stays
	// clean and delay-loaded imports resolve to this DLL when first invoked.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AtlasUE"));
	const FString BaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();
	const FString ThirdPartyDir = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(BaseDir, TEXT("ThirdParty"), TEXT("AtlasNetClient"), TEXT("Win64")));
	const FString DllPath = FPaths::Combine(ThirdPartyDir, TEXT("atlas_net_client.dll"));

	// Push the ThirdParty dir onto the DLL search path so Windows resolves mimalloc
	// (and any future transitive dep) from next to atlas_net_client.dll itself.
	FPlatformProcess::PushDllDirectory(*ThirdPartyDir);
	NetClientDllHandle = FPlatformProcess::GetDllHandle(*DllPath);
	FPlatformProcess::PopDllDirectory(*ThirdPartyDir);
	if (NetClientDllHandle == nullptr)
	{
		UE_LOG(LogAtlasUE, Error, TEXT("failed to load atlas_net_client.dll at %s"), *DllPath);
		return;
	}

	const uint32 AbiVersion = AtlasNetGetAbiVersion();
	UE_LOG(LogAtlasUE, Log, TEXT("AtlasUE module started; atlas_net_client ABI=0x%08X"), AbiVersion);
}

void FAtlasUEModule::ShutdownModule()
{
	if (NetClientDllHandle != nullptr)
	{
		FPlatformProcess::FreeDllHandle(NetClientDllHandle);
		NetClientDllHandle = nullptr;
	}
	UE_LOG(LogAtlasUE, Log, TEXT("AtlasUE module stopped"));
}

IMPLEMENT_MODULE(FAtlasUEModule, AtlasUE)
