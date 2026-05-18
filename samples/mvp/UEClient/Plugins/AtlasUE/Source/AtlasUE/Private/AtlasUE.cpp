#include "AtlasUE.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#include "entitydef/entitydef_api.h"
#include "net_client/client_api.h"

DEFINE_LOG_CATEGORY(LogAtlasUE);
DEFINE_LOG_CATEGORY_STATIC(LogAtlasNetSdk, Log, All);

namespace
{
AtlasEdrContext* g_edr_ctx = nullptr;

void AtlasNetLogForward(int32_t Level, const char* Message, int32_t Len)
{
	if (Message == nullptr || Len <= 0) return;
	const FString Msg = FString::ConstructFromPtrSize(UTF8_TO_TCHAR(Message), Len);
	switch (Level)
	{
		case 0: UE_LOG(LogAtlasNetSdk, VeryVerbose, TEXT("%s"), *Msg); break;
		case 1: UE_LOG(LogAtlasNetSdk, Verbose,     TEXT("%s"), *Msg); break;
		case 2: UE_LOG(LogAtlasNetSdk, Log,         TEXT("%s"), *Msg); break;
		case 3: UE_LOG(LogAtlasNetSdk, Warning,     TEXT("%s"), *Msg); break;
		default: UE_LOG(LogAtlasNetSdk, Error,      TEXT("%s"), *Msg); break;
	}
}

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

void LoadEntityDefRegistry(const FString& BaseDir)
{
	g_edr_ctx = AtlasEdrCreate(ATLAS_EDR_ABI_VERSION);
	if (g_edr_ctx == nullptr)
	{
		UE_LOG(LogAtlasUE, Error, TEXT("AtlasEdrCreate failed: %s"),
			UTF8_TO_TCHAR(AtlasEdrGlobalLastError()));
		return;
	}
	const FString AtdfPath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(BaseDir, TEXT("ThirdParty"), TEXT("AtlasEntityDef"), TEXT("entity_defs.bin")));
	const auto AtdfBytes = StringCast<ANSICHAR>(*AtdfPath);
	const int32 LoadResult = AtlasEdrLoadFromFile(g_edr_ctx, AtdfBytes.Get());
	if (LoadResult != ATLAS_EDR_OK)
	{
		UE_LOG(LogAtlasUE, Error, TEXT("AtlasEdrLoadFromFile %s failed (%d): %s"),
			*AtdfPath, LoadResult, UTF8_TO_TCHAR(AtlasEdrLastError(g_edr_ctx)));
		AtlasEdrDestroy(g_edr_ctx);
		g_edr_ctx = nullptr;
		return;
	}
	UE_LOG(LogAtlasUE, Log, TEXT("Loaded ATDF from %s; digest_size=%d"),
		*AtdfPath, AtlasEdrGetDigestSize(g_edr_ctx));
}
}  // namespace

AtlasEdrContext* FAtlasUEModule::GetEdrContext() { return g_edr_ctx; }

void FAtlasUEModule::StartupModule()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AtlasUE"));
	const FString BaseDir = Plugin.IsValid() ? Plugin->GetBaseDir() : FString();

	NetClientDllHandle = LoadThirdPartyDll(BaseDir, TEXT("AtlasNetClient"), TEXT("atlas_net_client.dll"));
	EntityDefDllHandle = LoadThirdPartyDll(BaseDir, TEXT("AtlasEntityDef"), TEXT("atlas_entitydef_client.dll"));

	// Either DLL missing means subsequent symbol lookups would dangle; release
	// whichever one did load so the plugin isn't stuck in a half-initialized
	// state where ShutdownModule is the only thing that cleans up.
	if (NetClientDllHandle == nullptr || EntityDefDllHandle == nullptr)
	{
		if (NetClientDllHandle != nullptr)
		{
			FPlatformProcess::FreeDllHandle(NetClientDllHandle);
			NetClientDllHandle = nullptr;
		}
		if (EntityDefDllHandle != nullptr)
		{
			FPlatformProcess::FreeDllHandle(EntityDefDllHandle);
			EntityDefDllHandle = nullptr;
		}
		return;
	}

	AtlasNetSetLogHandler(&AtlasNetLogForward);

	const uint32 NetAbi = AtlasNetGetAbiVersion();
	const uint32 EdrAbi = AtlasEdrGetAbiVersion();
	UE_LOG(LogAtlasUE, Log,
		TEXT("AtlasUE module started; atlas_net_client ABI=0x%08X, atlas_entitydef_client ABI=0x%08X"),
		NetAbi, EdrAbi);

	LoadEntityDefRegistry(BaseDir);
}

void FAtlasUEModule::ShutdownModule()
{
	if (g_edr_ctx != nullptr)
	{
		AtlasEdrDestroy(g_edr_ctx);
		g_edr_ctx = nullptr;
	}
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
