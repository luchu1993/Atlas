#include "AtlasUE.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogAtlasUE);

void FAtlasUEModule::StartupModule()
{
	UE_LOG(LogAtlasUE, Log, TEXT("AtlasUE module started"));
}

void FAtlasUEModule::ShutdownModule()
{
	UE_LOG(LogAtlasUE, Log, TEXT("AtlasUE module stopped"));
}

IMPLEMENT_MODULE(FAtlasUEModule, AtlasUE)
