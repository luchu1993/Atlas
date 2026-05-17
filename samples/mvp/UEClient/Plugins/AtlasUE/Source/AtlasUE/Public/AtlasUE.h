#pragma once

#include "Modules/ModuleInterface.h"

struct AtlasEdrContext;

DECLARE_LOG_CATEGORY_EXTERN(LogAtlasUE, Log, All);

class FAtlasUEModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	// Process-wide handle to the EntityDefRegistry loaded at module startup.
	// Returns nullptr if ATDF load failed; callers should null-check.
	static AtlasEdrContext* GetEdrContext();

private:
	void* NetClientDllHandle = nullptr;
	void* EntityDefDllHandle = nullptr;
};
