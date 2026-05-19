#include "AtlasHudWidget.h"

#include "Engine/GameInstance.h"

UAtlasHudWidget::UAtlasHudWidget(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UAtlasHudWidget::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
	Super::NativeTick(Geometry, DeltaTime);

	// Exponential smoothing on instantaneous fps so the UI text doesn't strobe
	// when a single frame stretches under GC / autosave.
	const float InstantFps = DeltaTime > 0.f ? 1.f / DeltaTime : 0.f;
	SmoothedFps = SmoothedFps == 0.f ? InstantFps : SmoothedFps * 0.9f + InstantFps * 0.1f;

	TimeSinceLastRefresh += DeltaTime;
	if (TimeSinceLastRefresh < RefreshIntervalSec) return;
	TimeSinceLastRefresh = 0.f;

	FAtlasHudFrameStats Stats;
	Stats.Fps = SmoothedFps;
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UAtlasSubsystem* Sub = GI->GetSubsystem<UAtlasSubsystem>())
		{
			Sub->GetNetStats(Stats.Net);
			Stats.NetState = Sub->GetNetState();
			Stats.PlayerEntityId = Sub->GetPlayerEntityId();
			if (UAtlasSpaceData* SD = Sub->GetSpaceData())
			{
				// space_data.def: npcCount id=1 type=int32 (space id 1).
				int32 Value = -1;
				if (SD->GetInt32(1, 1, Value)) Stats.NpcCount = Value;
			}
		}
	}
	OnHudRefresh(Stats);
}
